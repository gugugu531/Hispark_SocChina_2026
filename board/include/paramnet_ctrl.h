#ifndef SOCCHINA_PARAMNET_CTRL_H
#define SOCCHINA_PARAMNET_CTRL_H

/* ParamNet 实时闭环一拍（路线 B2）：
 *   chn2 缩略图(NV21) → ParamNet OM(AIPP 内建) → u[30]
 *   → paramnet_u_to_theta → paramnet_theta_to_blob → isp_apply_blob_buffer(热刷新)
 *
 * 前提：infer_init() 已用部署 OM（models/weights/paramnet/paramnet_256x144_aipp.om）
 * 加载 ParamNet；frame_info 是 256x144 NV21 控制帧（与 OM 的 AIPP 输入尺寸一致）。
 * 由 control worker 在刷新判决通过时调用（低频，~场景变化触发）。
 *
 * infer/isp 依赖均有 SDK-free 桩，故本模块 SDK-free 构建下亦可编译（返回错误）。 */

/* 运行一拍并热刷新 ISP。返回 0 成功；<0 为推理/blob/施加失败。
 * frame_info 类型为 ot_video_frame_info*（void* 避免头部引入 SDK 类型）。 */
int paramnet_run_and_apply(const void *frame_info);

#endif /* SOCCHINA_PARAMNET_CTRL_H */
