#include "Processes.r"
/* Memory partition: 8 MB ROM, tables and the recompiled games' code */
resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    doesActivateOnFGSwitch, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    128 * 1024 * 1024, 112 * 1024 * 1024
};
