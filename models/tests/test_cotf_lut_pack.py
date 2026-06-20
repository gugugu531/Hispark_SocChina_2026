"""CoTF NN→硬件 LUT 打包桥的单元测试。"""

import os

import numpy as np
import pytest

from models.tools.cotf_lut_pack import (
    HW_BANK_COUNTS,
    HW_LUT_LENGTH,
    decode_paramnet_output,
    identity_cubic_lut,
    pack_cubic_to_hw,
    pack_u30,
    resample_to_hw_mesh,
    unpack_hw_mesh,
    write_lut_bin,
)


def test_pack_u30_range_and_dtype():
    lut = identity_cubic_lut(9).reshape(-1, 3)
    packed = pack_u30(lut)
    assert packed.dtype == np.uint32
    assert packed.max() <= (1 << 30) - 1  # 硬件 u32 上限 2^30-1


def test_pack_u30_endpoints():
    # 纯黑 → 0；纯白 → 三个 10bit 全 1 = 2^30-1
    assert pack_u30(np.array([[0.0, 0.0, 0.0]]))[0] == 0
    assert pack_u30(np.array([[1.0, 1.0, 1.0]]))[0] == (1 << 30) - 1


def test_pack_u30_bit_layout_rgb():
    # R=1023(0x3ff) G=0 B=0 → bits[29:20] set
    v = pack_u30(np.array([[1.0, 0.0, 0.0]]), order="rgb")[0]
    assert v == (1023 << 20)
    assert ((v >> 20) & 0x3FF) == 1023 and ((v >> 10) & 0x3FF) == 0 and (v & 0x3FF) == 0


def test_pack_u30_default_layout_is_board_verified_rgb():
    v = pack_u30(np.array([[1.0, 0.0, 0.0]]))[0]
    assert v == (1023 << 20)


def test_resample_to_hw_mesh_length():
    cubic = identity_cubic_lut(17)
    mesh = resample_to_hw_mesh(cubic)
    assert mesh.shape == (HW_LUT_LENGTH, 3)


def test_pack_cubic_to_hw_full_chain():
    packed = pack_cubic_to_hw(identity_cubic_lut(17))
    assert packed.shape == (HW_LUT_LENGTH,)
    assert packed.dtype == np.uint32
    assert packed.max() <= (1 << 30) - 1


def test_identity_resample_roundtrip():
    mesh = resample_to_hw_mesh(identity_cubic_lut(17))
    assert HW_BANK_COUNTS == (729, 648, 648, 576, 648, 576, 576, 512)
    # 前四个值来自 bank0..3；下一组再次是 bank0..3。
    assert np.allclose(mesh[0], [0, 0, 0])
    assert np.allclose(mesh[1], [1 / 16, 0, 0])
    assert np.allclose(mesh[2], [0, 1 / 16, 0])
    assert np.allclose(mesh[3], [1 / 16, 1 / 16, 0])
    assert np.allclose(mesh[4], [2 / 16, 0, 0])
    # bank1 和 bank3 的短尾由官方打印函数补零。
    assert np.all(mesh[4 * 648 + 1 : 4 * 729 : 4] == 0)
    assert np.all(mesh[4 * 576 + 3 : 4 * 729 : 4] == 0)
    assert np.allclose(unpack_hw_mesh(mesh), identity_cubic_lut(17))


def test_decode_paramnet_output_shape():
    flat = np.random.rand(3 * 9 ** 3)
    cubic = decode_paramnet_output(flat, 9)
    assert cubic.shape == (9, 9, 9, 3)
    assert cubic.min() >= 0.0 and cubic.max() <= 1.0


def test_write_lut_bin(tmp_path):
    packed = pack_cubic_to_hw(identity_cubic_lut(17))
    p = str(tmp_path / "clut.bin")
    write_lut_bin(p, packed)
    assert os.path.getsize(p) == HW_LUT_LENGTH * 4
    back = np.fromfile(p, dtype="<u4")
    assert back.size == HW_LUT_LENGTH and np.array_equal(back, packed)


def test_write_lut_bin_rejects_wrong_length(tmp_path):
    with pytest.raises(ValueError):
        write_lut_bin(str(tmp_path / "bad.bin"), np.zeros(100, dtype=np.uint32))
