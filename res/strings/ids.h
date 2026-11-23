#pragma once

// AuraLite-Defrag String Table & Resource IDs (Clean-Room Original IDs)
#define IDI_AURALITE_APP              101

#define IDC_DRIVE_LIST                201
#define IDC_MAIN_TABS                 202
#define IDC_FILE_LIST                 203
#define IDC_STATUS_BAR                204
#define IDC_TOOLBAR                   205

#define IDM_FILE_OPEN_VOLUME          301
#define IDM_FILE_EXPORT_PNG           302
#define IDM_FILE_EXPORT_CSV           303
#define IDM_FILE_EXIT                 304

#define IDM_ACTION_ANALYZE            310
#define IDM_ACTION_DEFRAG             311
#define IDM_ACTION_QUICK              312
#define IDM_ACTION_FREE_SPACE         313
#define IDM_ACTION_MOVE_LARGE         314
#define IDM_ACTION_DEFRAG_SELECTED    315
#define IDM_ACTION_PAUSE              316
#define IDM_ACTION_RESUME             317
#define IDM_ACTION_STOP               318
#define IDM_ACTION_DRY_RUN            319
#define IDM_ACTION_MFT_OPT            320

#define IDM_VIEW_DRIVE_MAP            330
#define IDM_VIEW_FILE_LIST            331
#define IDM_VIEW_REFRESH              332
#define IDM_VIEW_TOGGLE_THEME         333
#define IDM_VIEW_ZOOM_1X              334
#define IDM_VIEW_ZOOM_4X              335
#define IDM_VIEW_ZOOM_16X             336

#define IDM_TOOLS_SETTINGS            340
#define IDM_TOOLS_EXCLUSIONS          341
#define IDM_TOOLS_SCHEDULER           342
#define IDM_TOOLS_HISTORY             343
#define IDM_TOOLS_DEMO_MODE           344

#define IDM_HELP_USER_GUIDE           350
#define IDM_HELP_ABOUT                351

// String table IDs matching src/core/strings.cpp
#define IDS_APP_TITLE                 1000
#define IDS_ELEVATION_OK              1001
#define IDS_ELEVATION_REQUIRED        1002
#define IDS_STATE_IDLE                1003
#define IDS_STATE_ANALYZING           1004
#define IDS_STATE_DEFRAGMENTING       1005
#define IDS_STATE_PAUSED              1006
#define IDS_OP_CANCELLED              1007
#define IDS_OP_STOPPED                1008
#define IDS_OP_COMPLETE               1009
#define IDS_RESUME_OK                 1010

#define IDS_SSD_BLOCKED_TITLE         1020
#define IDS_SSD_BLOCKED_BODY          1021
#define IDS_SSD_UNKNOWN_BODY          1022
#define IDS_SSD_ANALYZE_ONLY          1023
#define IDS_SSD_CLI_REFUSAL           1024
#define IDS_SSD_NO_OVERRIDE           1025

#define IDS_MFT_TIER1_OK              1030
#define IDS_MFT_TIER1_UNAVAILABLE     1031
#define IDS_MFT_TIER1_FAILED          1032
#define IDS_MFT_TIER2_APPLIED         1033
#define IDS_MFT_TIER3_REPORT_ONLY     1034
#define IDS_MFT_TIER4_UNAVAILABLE     1035
#define IDS_MFT_CONFIRM               1036
#define IDS_MFT_ZONE_GUIDANCE         1037
#define IDS_MFT_ZONE_EVACUATED        1038
#define IDS_MFT_SKIPPED               1039

#define IDS_PREFLIGHT_FAILED_GENERIC  1050
#define IDS_PREFLIGHT_READ_ONLY       1051
#define IDS_PREFLIGHT_BITLOCKER       1052
#define IDS_PREFLIGHT_UNSUPPORTED_FS  1053
#define IDS_DIRTY_VOLUME              1054
#define IDS_DRY_RUN_HEADER            1055
#define IDS_JOURNAL_RECOVERY          1056
#define IDS_DEMO_BANNER               1057

#define IDS_ERR_ANALYSIS              1070
#define IDS_ERR_ACCESS                1071
#define IDS_ERR_STALE_BITMAP          1072
#define IDS_ERR_MOVE_GENERIC          1073
#define IDS_ERR_UNSUPPORTED_STREAM    1074
#define IDS_ERR_FILE_VANISHED         1075
#define IDS_ERR_VERIFY                1076
#define IDS_ERR_JOURNAL               1077
#define IDS_ERR_CONFIG                1078
#define IDS_ERR_EXPORT                1079
#define IDS_ERR_PATH                  1080
#define IDS_ERR_VOLUME_REMOVED        1081
#define IDS_ERR_INTERNAL              1082

#define IDS_TAB_DRIVE_MAP             1100
#define IDS_TAB_FILE_LIST             1101
#define IDS_BTN_ANALYZE               1102
#define IDS_BTN_DEFRAG                1103
#define IDS_BTN_PAUSE                 1104
#define IDS_BTN_STOP                  1105
#define IDS_BTN_QUICK                 1106
#define IDS_BTN_FREE_SPACE            1107
