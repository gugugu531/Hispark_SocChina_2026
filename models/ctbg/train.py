"""P1 双向训练入口(PyTorch Lightning)。
满足要求:成熟框架 + 训练数据 + TensorBoard/CSV 日志 + 断点续训(--resume)+ 种子固定。

用法:
  小训冒烟:python p1_bidir/train.py --exp-name smoke --max-epochs 3 --samples-per-epoch 200
  正式训练:python p1_bidir/train.py
  断点续训:python p1_bidir/train.py --resume last      # 自动找该 exp 的 last.ckpt
            python p1_bidir/train.py --resume <path/to/xxx.ckpt>
"""
import sys, argparse
from pathlib import Path
import yaml
import torch
import lightning as L
from lightning.pytorch.loggers import TensorBoardLogger, CSVLogger
from lightning.pytorch.callbacks import ModelCheckpoint, LearningRateMonitor

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from data import BidirDataModule
from litmodule import LitMobileIEBidir
from litmodule_coeff import LitMobileIECoeff, LitCTBG, LitCTBG_6ch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=str(HERE / "config.yaml"))
    ap.add_argument("--resume", default=None, help="'last' 或 ckpt 路径")
    ap.add_argument("--exp-name", default=None)
    ap.add_argument("--max-epochs", type=int, default=None)
    ap.add_argument("--samples-per-epoch", type=int, default=None)
    ap.add_argument("--num-workers", type=int, default=None)
    ap.add_argument("--save-every", type=int, default=None)
    ap.add_argument("--model", choices=["image", "coeff", "ctbg", "ctbg6ch"], default="image")
    ap.add_argument("--p-nonuniform", type=float, default=None)
    args = ap.parse_args()

    cfg = yaml.safe_load(open(args.config))
    for k, v in [("exp_name", args.exp_name), ("max_epochs", args.max_epochs),
                 ("samples_per_epoch", args.samples_per_epoch), ("num_workers", args.num_workers),
                 ("save_every_n_epochs", args.save_every), ("p_nonuniform", args.p_nonuniform)]:
        if v is not None:
            cfg[k] = v

    # 种子固定(torch/numpy/random + DataLoader worker;cudnn 确定性)
    L.seed_everything(cfg["seed"], workers=True)
    torch.backends.cudnn.benchmark = False
    torch.backends.cudnn.deterministic = True

    run_dir = Path(cfg["out_dir"]) / cfg["exp_name"]
    run_dir.mkdir(parents=True, exist_ok=True)
    yaml.safe_dump(cfg, open(run_dir / "config_used.yaml", "w"), allow_unicode=True)

    dm = BidirDataModule(cfg)
    if args.model == "ctbg6ch":
        lit = LitCTBG_6ch(channels=cfg["channels"], rep_scale=cfg["rep_scale"],
                          down=cfg.get("down", 4), lr=cfg["lr"], max_epochs=cfg["max_epochs"],
                          tv_w=cfg.get("tv_w", 0.001), distill_w=cfg.get("distill_w", 0.0),
                          teacher_pkl=cfg.get("teacher_pkl"), b_range=cfg.get("b_range", 0.02),
                          std_w=cfg.get("std_w", 0.1), sat_w=cfg.get("sat_w", 0.1),
                          illum_w=cfg.get("illum_w", 0.0),
                          residual_spatial=cfg.get("residual_spatial", True),
                          weight_decay=cfg.get("weight_decay", 0.0),
                          t_0=cfg.get("t_0", 40))
        # warm-start: 从 v8 checkpoint 加载 backbone 权重
        warm_ckpt = cfg.get("warm_start_ckpt", None)
        if warm_ckpt and not args.resume:
            print(f"[warm-start] 从 {warm_ckpt} 加载 backbone 权重...")
            ckpt = torch.load(warm_ckpt, map_location="cpu", weights_only=True)
            state = ckpt["state_dict"]
            # 过滤: 仅加载 backbone(bb.)和 film 层,跳过 tail(维度不同 18→6ch)
            warm_state = {k: v for k, v in state.items()
                          if k.startswith("net.bb.") and "tail" not in k}
            warm_state.update({k: v for k, v in state.items()
                               if k.startswith("net.film_")})
            # 映射到当前模型 key
            mapped = {}
            for k, v in warm_state.items():
                mapped[k] = v  # key 结构相同(net.bb.*, net.film_*)
            missing, unexpected = lit.load_state_dict(mapped, strict=False)
            print(f"[warm-start] loaded {len(mapped)} params, "
                  f"missing={len(missing)} (tail+head 随机初始化), unexpected={len(unexpected)}")
            if len(unexpected) > 0:
                print(f"[warm-start] unexpected keys: {unexpected[:5]}")
    elif args.model == "ctbg":
        lit = LitCTBG(channels=cfg["channels"], rep_scale=cfg["rep_scale"],
                      down=cfg.get("down", 4), lr=cfg["lr"], max_epochs=cfg["max_epochs"],
                      tv_w=cfg.get("tv_w", 0.001), distill_w=cfg.get("distill_w", 0.0),
                      teacher_pkl=cfg.get("teacher_pkl"), b_range=cfg.get("b_range", 0.02),
                      std_w=cfg.get("std_w", 0.1), sat_w=cfg.get("sat_w", 0.1),
                      illum_w=cfg.get("illum_w", 0.0),
                      residual_spatial=cfg.get("residual_spatial", True))
    elif args.model == "coeff":
        lit = LitMobileIECoeff(channels=cfg["channels"], rep_scale=cfg["rep_scale"],
                               down=cfg.get("down", 4), lr=cfg["lr"], max_epochs=cfg["max_epochs"],
                               tv_w=cfg.get("tv_w", 0.002), gamma=cfg.get("gamma", True),
                               distill_w=cfg.get("distill_w", 0.0), teacher_pkl=cfg.get("teacher_pkl"),
                               b_range=cfg.get("b_range", 0.2), std_w=cfg.get("std_w", 0.0),
                               sat_w=cfg.get("sat_w", 0.0))
    else:
        lit = LitMobileIEBidir(channels=cfg["channels"], rep_scale=cfg["rep_scale"],
                               lr=cfg["lr"], max_epochs=cfg["max_epochs"])

    ckpt_cb = ModelCheckpoint(
        dirpath=run_dir / "ckpt", filename="best-{epoch:04d}-{val_psnr:.2f}",
        monitor="val_psnr", mode="max", save_top_k=2, save_last=True,
        every_n_epochs=cfg.get("save_every_n_epochs", 20))
    callbacks = [ckpt_cb, LearningRateMonitor(logging_interval="epoch")]
    loggers = [TensorBoardLogger(cfg["out_dir"], name=cfg["exp_name"]),
               CSVLogger(cfg["out_dir"], name=cfg["exp_name"])]

    trainer = L.Trainer(
        max_epochs=cfg["max_epochs"], accelerator="auto", devices=1,
        precision=cfg.get("precision", 32), logger=loggers, callbacks=callbacks,
        deterministic="warn", log_every_n_steps=10,
        check_val_every_n_epoch=1)

    # 断点续训
    ckpt_path = args.resume
    if ckpt_path == "last":
        last = run_dir / "ckpt" / "last.ckpt"
        ckpt_path = str(last) if last.exists() else None
        print(f"[resume] {'从 ' + str(last) if ckpt_path else '无 last.ckpt,从头训练'}")

    trainer.fit(lit, dm, ckpt_path=ckpt_path)
    print(f"[done] best val_psnr={ckpt_cb.best_model_score} @ {ckpt_cb.best_model_path}")


if __name__ == "__main__":
    main()
