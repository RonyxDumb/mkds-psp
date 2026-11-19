#include "Main/System/main.h"
#include "Main/System/startup.h"
#include "Main/Scene/scene_run_forever.h"

void GameMain(void) {
    InitStartup();
    RunRootHeapCall((u32) InitAndRunScenesForever);
}
