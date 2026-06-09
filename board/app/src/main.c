#include "control.h"
#include "log.h"
#include "version.h"

/*
 * 板端应用骨架入口（生产主程序）。
 * 当前仅验证统一构建/部署闭环并演示模块调用；
 * 数据通路各级（采集/ISP/缩放/预处理/推理/后处理/合成/显示/串流）随开发接入。
 */
int main(void)
{
    LOG_INFO("%s v%s — board app skeleton", SOCCHINA_APP_NAME, SOCCHINA_APP_VERSION);
#ifdef WITH_SS928_SDK
    LOG_INFO("built with SS928 SDK");
#else
    LOG_INFO("built without SS928 SDK (toolchain-only smoke)");
#endif

    /* 演示：控制模块判决 */
    luma_stats_t demo = {30.0f, 0.1f, 25.0f};
    LOG_INFO("control_decide(demo) = %d", control_decide(&demo));
    return 0;
}
