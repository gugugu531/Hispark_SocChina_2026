"""硬件 θ* 黑盒搜索——真实 ISP 上逐图搜最优参数(蒸馏标签生成原型 + 上限诊断)。

两轮批处理搜索(与 test_raw_replay --blob-dir 批量模式配合):
    python -m models.isp_simulator.hw_search gen1 <r1_dir>      # 64 宽范围候选
    # 板端: test_raw_replay --raw-file ... --blob-dir <r1_dir> → 拉回
    python -m models.isp_simulator.hw_search eval <board_out> 64 R1 <r1_dir>/results.json
    python -m models.isp_simulator.hw_search gen2 <r1_results> <r1_dir>/cands.pt <r2_dir>
    # 板端第二轮 → eval 同上

首轮应用(2026-07-04 差距归因,docs/isp-param-tuning-research.md §5.9):
欠曝图 ParamNet 已达 31 维硬件上限(结构性),过曝图有 3.5-4dB 可修空间。
"""
import numpy as np, torch, sys, json
from pathlib import Path
from PIL import Image
from models.isp_simulator import make_identity_params
from models.isp_simulator.params import get_offset
from models.isp_simulator.isp_blob import sim_params_to_blob
from models.isp_simulator.fidelity_gate import load_nv21_rgb
from models.isp_simulator.calib_dataset import lhs

D = Path("models/weights/lcdp_replay")  # GT 参考目录(可按需改)
TAGS = ["under1", "under2", "over1", "over2"]

def wide_sample(num, rng):
    """宽范围采样(tone 对称 ±0.30,可提亮可压暗)→ (num,97)。"""
    u = lhs(num, 14, rng)
    params = make_identity_params(num)
    off_t = get_offset("drc_tone")
    base = torch.linspace(0, 1, 6)
    for i in range(num):
        cp = base.clone()
        cp[1:5] = (cp[1:5] + torch.from_numpy(u[i, 0:4]).float()*0.60 - 0.30).clamp(0, 1)
        cp = torch.cummax(cp, 0).values; cp[0], cp[5] = 0, 1
        params[i, off_t:off_t+6] = cp
    params[:, get_offset("drc_strength")] = torch.from_numpy(u[:, 4]).float()
    off_l = get_offset("ldci")
    lo = torch.tensor([0., .2, .1, 0., .2, .4, 0., .2]); sp = torch.tensor([.9, .6, .5, .6, .6, .5, .5, .6])
    params[:, off_l:off_l+8] = lo + torch.from_numpy(u[:, 5:13]).float()*sp
    params[:, get_offset("drc_blend")] = 0.2 + torch.from_numpy(u[:, 13]).float()*0.6
    return params

def perturb(base_params, num, sigma, rng):
    """围绕 base 的邻域扰动(在采样维上加噪,tone 重投影单调)。"""
    out = base_params.repeat(num, 1).clone()
    off_t = get_offset("drc_tone")
    noise_t = torch.from_numpy(rng.normal(0, sigma*0.3, (num, 4))).float()
    cp = out[:, off_t+1:off_t+5] + noise_t
    full = torch.cat([torch.zeros(num,1), cp.clamp(0,1), torch.ones(num,1)], 1)
    out[:, off_t:off_t+6] = torch.cummax(full, 1).values
    for key, dim in [("drc_strength",1), ("drc_blend",1)]:
        off = get_offset(key)
        out[:, off:off+dim] = (out[:, off:off+dim] +
            torch.from_numpy(rng.normal(0, sigma, (num, dim))).float()).clamp(0, 1)
    off_l = get_offset("ldci")
    out[:, off_l:off_l+8] = (out[:, off_l:off_l+8] +
        torch.from_numpy(rng.normal(0, sigma*0.5, (num, 8))).float()).clamp(0, 1)
    return out

def write_blobs(params, outdir):
    outdir = Path(outdir); outdir.mkdir(parents=True, exist_ok=True)
    for f in outdir.glob("*.bin"): f.unlink()
    for i in range(params.shape[0]):
        (outdir/f"s{i:03d}.bin").write_bytes(sim_params_to_blob(params[i:i+1]))
    torch.save(params, outdir/"cands.pt")
    print(f"{params.shape[0]} 候选 -> {outdir}")

def psnr(a, b):
    mse = float(((a-b)**2).mean())
    return 10*np.log10(1/max(mse, 1e-10))

def evaluate(board_dir, num_cands, round_tag, ref_suffix="gt"):
    board_dir = Path(board_dir)
    results = {}
    for fi, t in enumerate(TAGS):
        gt = np.asarray(Image.open(D/f"{t}_{ref_suffix}.png").resize((512, 288)), np.float32)/255.0
        scores = []
        for i in range(num_cands):
            hw = load_nv21_rgb(board_dir/f"out_f{fi:02d}_{i+1:02d}_blob_s{i:03d}.bin.nv21", 512, 288)
            scores.append(psnr(hw, gt))
        scores = np.array(scores)
        order = np.argsort(scores)[::-1]
        results[t] = {"best_idx": int(order[0]), "best_psnr": float(scores[order[0]]),
                      "top4": [int(j) for j in order[:4]],
                      "top4_psnr": [float(scores[j]) for j in order[:4]]}
        print(f"[{round_tag}] {t}: best s{order[0]:03d} = {scores[order[0]]:.2f} dB "
              f"(top4: {[f'{scores[j]:.2f}' for j in order[:4]]})")
    return results

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "gen1":
        rng = np.random.default_rng(1)
        write_blobs(wide_sample(64, rng), sys.argv[2])
    elif cmd == "eval":
        ref = sys.argv[6] if len(sys.argv) > 6 else "gt"
        r = evaluate(sys.argv[2], int(sys.argv[3]), sys.argv[4], ref_suffix=ref)
        Path(sys.argv[5]).write_text(json.dumps(r, indent=2))
    elif cmd == "gen2":
        prev = json.loads(Path(sys.argv[2]).read_text())
        cands = torch.load(sys.argv[3])
        rng = np.random.default_rng(2)
        pool = []
        for t in TAGS:
            for rank, j in enumerate(prev[t]["top4"][:2]):
                pool.append(perturb(cands[j:j+1], 8, 0.08 if rank == 0 else 0.15, rng))
        write_blobs(torch.cat(pool, 0), sys.argv[4])
