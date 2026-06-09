#include <stdio.h>

#include "version.h"

/*
 * 板端应用骨架入口。
 * 当前仅验证统一构建/部署闭环；数据通路各级（采集/ISP/缩放/预处理/推理/
 * 后处理/合成/显示/串流/控制）随开发推进接入。
 */
int main(void)
{
    printf("%s v%s — board app skeleton\n", SOCCHINA_APP_NAME, SOCCHINA_APP_VERSION);
#ifdef WITH_SS928_SDK
    printf("built with SS928 SDK\n");
#else
    printf("built without SS928 SDK (toolchain-only smoke build)\n");
#endif
    return 0;
}
