#include <furi.h>
#include <gui/gui.h>
#include <gui/icon_i.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>
#include <gui/modules/dialog_ex.h>

#include <furi_hal_spi.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <toolbox/path.h>
#include <gui/modules/widget.h>




#include "sd_spi.h"

#define TAG "sd-spi-app"

/* generated by fbt from .png files in images folder */
// #include <sd_spi_app_icons.h>

#define TEXT_BOX_STORE_SIZE (4096)
#define PASSWORD_MAX_LEN (16)
#define ALERT_MAX_LEN 32

#define STORAGE_LOCKED_FILE "pwd.txt"

/** ids for all scenes used by the app */
typedef enum {
    AppScene_MainMenu,
    AppScene_Status,
    AppScene_Confirmation,
    AppScene_Password,
    AppScene_Info,
    AppScene_count
} AppScene;

/** ids for the 2 types of view used by the app */
typedef enum { AppView_Menu, AppView_Status, AppView_Dialog, AppView_TextInput, AppView_Info} SDSPIAppView;

/** the app context struct */
typedef struct {
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    TextBox* tb_status;
    Widget* widget_about;
    TextInput* text_input;
    DialogEx* dialog;
    char* input_pwd;
} SDSPIApp;

/** all custom events */
typedef enum { AppEvent_Status, AppEvent_Confirmation, AppEvent_Password, AppEvent_Info } AppEvent;

/** indices for menu items */
typedef enum { AppMenuSelection_Init, AppMenuSelection_Status, AppMenuSelection_SDLock, AppMenuSelection_SDUnLock, Confirmation_Dialog, AppMenuSelection_Password, AppMenuSelection_Info } AppMenuSelection;

bool notify_sequence(SdSpiStatus status){
  NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
  if(status == SdSpiStatusOK){ notification_message(notifications, &sequence_success); return true; }
  else{ notification_message(notifications, &sequence_error); }
  return false;
  furi_record_close(RECORD_NOTIFICATION);
}

/** main menu callback - sends a custom event to the scene manager based on the menu selection */
void app_menu_callback_main_menu(void* context, uint32_t index) {
    FURI_LOG_T(TAG, "app_menu_callback_main_menu");
    SDSPIApp* app = context;
    switch(index) {
    case AppMenuSelection_Status:
      scene_manager_handle_custom_event(app->scene_manager, AppEvent_Status);
      break;
    case AppMenuSelection_Init:
      {
        SdSpiStatus sdStatus;
        sdStatus = sd_init(false);
        notify_sequence(sdStatus);
      }
      break;
    case AppMenuSelection_SDLock:
      FURI_LOG_T(TAG, "AppMenuSelection_SDLock");
      {
        SdSpiStatus sdStatus;
        sdStatus = sd_set_pwd(app->input_pwd);
        notify_sequence(sdStatus);
      }

      break;
    case AppMenuSelection_SDUnLock:
      FURI_LOG_T(TAG, "AppMenuSelection_SDUnLock");
      {
        SdSpiStatus sdStatus;// = sd_init(false);
        sdStatus = sd_clr_pwd(app->input_pwd);
        notify_sequence(sdStatus);
      }
      break;
    case AppMenuSelection_Password:
      scene_manager_handle_custom_event(app->scene_manager, AppEvent_Password);
      break;
    case Confirmation_Dialog:
      scene_manager_handle_custom_event(app->scene_manager, AppEvent_Confirmation);
      break;
    case AppMenuSelection_Info:
      scene_manager_handle_custom_event(app->scene_manager, AppEvent_Info);
      break;
    }
}

/** resets the menu, gives it content, callbacks and selection enums */
void app_scene_on_enter_main_menu(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_enter_main_menu");
    SDSPIApp* app = context;
    submenu_reset(app->menu);

    submenu_add_item(
      app->menu,
      "SD Init",
      AppMenuSelection_Init,
      app_menu_callback_main_menu,
      app);
    submenu_add_item(
      app->menu,
      "SD Status",
      AppMenuSelection_Status,
      app_menu_callback_main_menu,
      app);
    submenu_add_item(
      app->menu,
      "SD Lock",
      AppMenuSelection_SDLock,
      app_menu_callback_main_menu,
      app);
    submenu_add_item(
      app->menu,
      "SD Unlock",
      AppMenuSelection_SDUnLock,
      app_menu_callback_main_menu,
      app);
    submenu_add_item(
      app->menu,
      "SD Force Erase",
      Confirmation_Dialog,
      app_menu_callback_main_menu,
      app);
    submenu_add_item(
      app->menu,
      "Password",
      AppMenuSelection_Password,
      app_menu_callback_main_menu,
      app);
    submenu_add_item(
      app->menu,
      "About",
      AppMenuSelection_Info,
      app_menu_callback_main_menu,
      app);

    view_dispatcher_switch_to_view(app->view_dispatcher, AppView_Menu);
}

/** main menu event handler - switches scene based on the event */
bool app_scene_on_event_main_menu(void* context, SceneManagerEvent event) {
    FURI_LOG_T(TAG, "app_scene_on_event_main_menu");
    SDSPIApp* app = context;
    bool consumed = false;
    switch(event.type) {
    case SceneManagerEventTypeCustom:
        switch(event.event) {
        case AppEvent_Status:
            scene_manager_next_scene(app->scene_manager, AppScene_Status);
            consumed = true;
            break;
        case AppEvent_Confirmation:
            scene_manager_next_scene(app->scene_manager, AppScene_Confirmation);
            consumed = true;
            break;
        case AppEvent_Password:
            scene_manager_next_scene(app->scene_manager, AppScene_Password);
            consumed = true;
            break;
        case AppEvent_Info:
            scene_manager_next_scene(app->scene_manager, AppScene_Info);
            consumed = true;
            break;
        }
        break;
    default:
        consumed = false;
        break;
    }
    return consumed;
}

void app_scene_on_exit_main_menu(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_exit_main_menu");
    SDSPIApp* app = context;
    submenu_reset(app->menu);
}

bool text_input_validator(const char* text, FuriString* error, void* context) {
    UNUSED(context);
    bool validated = true;
    if(strlen(text) > PASSWORD_MAX_LEN || strlen(text) < 1) {
        furi_string_set(error, "the pwd\nmust have\nfrom 1 to\n16 chars");
        validated = false;
    }
    return validated;
}
void text_input_done_callback(void* context){
  SDSPIApp* app = context;
  Storage* storage = furi_record_open(RECORD_STORAGE);
  FuriString* path;
  path = furi_string_alloc();
  furi_string_set_str(path, EXT_PATH("apps_data/sdspi"));
  if(!storage_file_exists(storage,EXT_PATH("apps_data"))) { storage_common_mkdir(storage, EXT_PATH("apps_data")); }
  if(!storage_file_exists(storage,EXT_PATH("apps_data/sdspi"))) { storage_common_mkdir(storage, EXT_PATH("apps_data/sdspi")); }

  path_append(path,STORAGE_LOCKED_FILE);
  File* file = storage_file_alloc(storage);
  storage_simply_remove(storage, furi_string_get_cstr(path));
  if(!storage_file_open(file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
      FURI_LOG_E(TAG, "Failed to open file");
  }
  if(!storage_file_write(file, app->input_pwd, strlen(app->input_pwd))) {
      FURI_LOG_E(TAG, "Failed to write to file");
  }
  furi_string_free(path);
  storage_file_close(file);
  storage_file_free(file);
  furi_record_close(RECORD_STORAGE);

  scene_manager_previous_scene(app->scene_manager);
}

/* App Scene Select Password */
void app_scene_on_enter_password(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_enter_password");
    SDSPIApp* app = context;
    // TextInput* text_input = app->text_input;
    text_input_set_header_text(app->text_input,"Enter password");
    text_input_set_validator(app->text_input, text_input_validator, context);
    text_input_set_result_callback(
        app->text_input,
        text_input_done_callback,
        app,
        app->input_pwd,
        PASSWORD_MAX_LEN+1,
        false);
    view_dispatcher_switch_to_view(app->view_dispatcher, AppView_TextInput);
}
bool app_scene_on_event_password(void* context, SceneManagerEvent event) {
    FURI_LOG_T(TAG, "app_scene_on_event_password");
    UNUSED(context);
    UNUSED(event);
    return false;
}
void app_scene_on_exit_password(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_exit_password");
    SDSPIApp* app = context;
    UNUSED(app);
    // text_input_reset(app->text_input);
    // text_input_set_validator(app->text_input, NULL, NULL);
}

/* App Scene Confirm SD Force Erase */
void app_dialog_erase_callback(DialogExResult result, void* context) {
  SDSPIApp* app = context;
  if(result == DialogExResultLeft) { scene_manager_previous_scene(app->scene_manager); }
  else if(result == DialogExResultRight) {
    {
      SdSpiStatus sdStatus;
      sdStatus = sd_force_erase();
      if(notify_sequence(sdStatus)){ scene_manager_previous_scene(app->scene_manager); };
    }
  }
}
void app_scene_on_enter_dialog(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_enter_dialog");
    SDSPIApp* app = context;
    dialog_ex_reset(app->dialog);

    dialog_ex_set_result_callback(app->dialog, app_dialog_erase_callback);
    dialog_ex_set_context(app->dialog, app);
    dialog_ex_set_left_button_text(app->dialog, "Back");
    dialog_ex_set_right_button_text(app->dialog, "Erase");
    // dialog_ex_set_center_button_text(app->dialog, "Menu List");
    dialog_ex_set_header(app->dialog, "Erase SD card?", 128/2, 12, AlignCenter, AlignTop);
    view_dispatcher_switch_to_view(app->view_dispatcher, AppView_Dialog);
}
bool app_scene_on_event_dialog(void* context, SceneManagerEvent event) {
    FURI_LOG_T(TAG, "app_scene_on_event_dialog");
    UNUSED(context);
    UNUSED(event);
    return false;
}
void app_scene_on_exit_dialog(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_exit_dialog");
    SDSPIApp* app = context;
    dialog_ex_reset(app->dialog);
}

/* App Scene About */
void app_scene_on_enter_info(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_enter_info");
    SDSPIApp* app = context;
    widget_reset(app->widget_about);
    widget_add_text_box_element(
        app->widget_about,
        0,
        2,
        128,
        14,
        AlignCenter,
        AlignBottom,
        "\e#\e!            SD Spi           \e!\n",
        false);
    FuriString* temp_str = furi_string_alloc();
    furi_string_cat_printf(temp_str, "Version: %s\n", VERSION_APP);
    furi_string_cat_printf(temp_str, "Developed by: %s\n", DEVELOPED);
    furi_string_cat_printf(temp_str, "Github: %s\n\n", GITHUB);

    widget_add_text_scroll_element(app->widget_about, 0, 16, 128, 50, furi_string_get_cstr(temp_str));
    furi_string_free(temp_str);
    view_dispatcher_switch_to_view(app->view_dispatcher, AppView_Info);
}
bool app_scene_on_event_info(void* context, SceneManagerEvent event) {
    FURI_LOG_T(TAG, "app_scene_on_event_info");
    UNUSED(context);
    UNUSED(event);
    return false;
}
void app_scene_on_exit_info(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_exit_info");
    SDSPIApp* app = context;
    widget_reset(app->widget_about);
}

/* App Scene SD Status */
void app_scene_on_enter_status(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_enter_status");
    SDSPIApp* app = context;
    text_box_reset(app->tb_status);

    FuriString* fs_status = furi_string_alloc();
    furi_string_reserve(fs_status, TEXT_BOX_STORE_SIZE);
    furi_string_set_char(fs_status, 0, 0);
    furi_string_set_str(fs_status, "Sd Status:");

    furi_string_cat_str(fs_status, "\nR1");
    if(cmd_answer.r1 != 0xff) {
      if(cmd_answer.r1 == SdSpi_R1_NO_ERROR){ furi_string_cat_str(fs_status, "\nNO_ERROR"); }
      if(cmd_answer.r1 & SdSpi_R1_ERASE_RESET){ furi_string_cat_str(fs_status, "\nERASE_RESET"); }
      if(cmd_answer.r1 & SdSpi_R1_IN_IDLE_STATE){ furi_string_cat_str(fs_status, "\nIN_IDLE_STATE"); }
      if(cmd_answer.r1 & SdSpi_R1_ILLEGAL_COMMAND){ furi_string_cat_str(fs_status, "\nILLEGAL_COMMAND"); }
      if(cmd_answer.r1 & SdSpi_R1_COM_CRC_ERROR){ furi_string_cat_str(fs_status, "\nCOM_CRC_ERROR"); }
      if(cmd_answer.r1 & SdSpi_R1_ERASE_SEQUENCE_ERROR){ furi_string_cat_str(fs_status, "\nERASE_SEQUENCE_ERROR"); }
      if(cmd_answer.r1 & SdSpi_R1_ADDRESS_ERROR){ furi_string_cat_str(fs_status, "\nADDRESS_ERROR"); }
      if(cmd_answer.r1 & SdSpi_R1_PARAMETER_ERROR){ furi_string_cat_str(fs_status, "\nPARAMETER_ERROR"); }
    }

    furi_string_cat_str(fs_status, "\nR2");
    if(cmd_answer.r2 != 0xff) {
      if(cmd_answer.r2 == SdSpi_R2_NO_ERROR){ furi_string_cat_str(fs_status, "\nNO_ERROR"); }
      if(cmd_answer.r2 & SdSpi_R2_CARD_LOCKED){ furi_string_cat_str(fs_status, "\nCARD_LOCKED"); }
      if(cmd_answer.r2 & SdSpi_R2_LOCKUNLOCK_ERROR){ furi_string_cat_str(fs_status, "\nLOCKUNLOCK_ERROR"); }
      if(cmd_answer.r2 & SdSpi_R2_ERROR){ furi_string_cat_str(fs_status, "\nERROR"); }
      if(cmd_answer.r2 & SdSpi_R2_CC_ERROR){ furi_string_cat_str(fs_status, "\nCC_ERROR"); }
      if(cmd_answer.r2 & SdSpi_R2_CARD_ECC_FAILED){ furi_string_cat_str(fs_status, "\nCARD_ECC_FAILED"); }
      if(cmd_answer.r2 & SdSpi_R2_WP_VIOLATION){ furi_string_cat_str(fs_status, "\nWP_VIOLATION"); }
      if(cmd_answer.r2 & SdSpi_R2_ERASE_PARAM){ furi_string_cat_str(fs_status, "\nERASE_PARAM"); }
      if(cmd_answer.r2 & SdSpi_R2_OUTOFRANGE){ furi_string_cat_str(fs_status, "\nOUTOFRANGE"); }
    }

    text_box_set_text(app->tb_status, furi_string_get_cstr(fs_status));
    text_box_set_focus(app->tb_status, TextBoxFocusEnd);
    furi_string_free(fs_status);
    view_dispatcher_switch_to_view(app->view_dispatcher, AppView_Status);
}
bool app_scene_on_event_status(void* context, SceneManagerEvent event) {
    FURI_LOG_T(TAG, "app_scene_on_event_status");
    UNUSED(context);
    UNUSED(event);
    return false;
}
void app_scene_on_exit_status(void* context) {
    FURI_LOG_T(TAG, "app_scene_on_exit_status");
    SDSPIApp* app = context;
    text_box_reset(app->tb_status);
}



/** collection of all scene on_enter handlers - in the same order as their enum */
void (*const app_scene_on_enter_handlers[])(void*) = {
    app_scene_on_enter_main_menu,
    app_scene_on_enter_status,
    // app_scene_on_enter_info,
    app_scene_on_enter_dialog,
    app_scene_on_enter_password,
    app_scene_on_enter_info};

/** collection of all scene on event handlers - in the same order as their enum */
bool (*const app_scene_on_event_handlers[])(void*, SceneManagerEvent) = {
    app_scene_on_event_main_menu,
    app_scene_on_event_status,
    // app_scene_on_event_info,
    app_scene_on_event_dialog,
    app_scene_on_event_password,
    app_scene_on_event_info};

/** collection of all scene on exit handlers - in the same order as their enum */
void (*const app_scene_on_exit_handlers[])(void*) = {
    app_scene_on_exit_main_menu,
    app_scene_on_exit_status,
    // app_scene_on_exit_info,
    app_scene_on_exit_dialog,
    app_scene_on_exit_password,
    app_scene_on_exit_info};

/** collection of all on_enter, on_event, on_exit handlers */
const SceneManagerHandlers app_scene_event_handlers = {
    .on_enter_handlers = app_scene_on_enter_handlers,
    .on_event_handlers = app_scene_on_event_handlers,
    .on_exit_handlers = app_scene_on_exit_handlers,
    .scene_num = AppScene_count};

/** custom event handler - passes the event to the scene manager */
bool app_scene_manager_custom_event_callback(void* context, uint32_t custom_event) {
    FURI_LOG_T(TAG, "app_scene_manager_custom_event_callback");
    furi_assert(context);
    SDSPIApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, custom_event);
}

/** navigation event handler - passes the event to the scene manager */
bool app_scene_manager_navigation_event_callback(void* context) {
    FURI_LOG_T(TAG, "app_scene_manager_navigation_event_callback");
    furi_assert(context);
    SDSPIApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

/** initialise the scene manager with all handlers */
void app_scene_manager_init(SDSPIApp* app) {
    FURI_LOG_T(TAG, "app_scene_manager_init");
    app->scene_manager = scene_manager_alloc(&app_scene_event_handlers, app);
}


/** initialise the views, and initialise the view dispatcher with all views */
void app_view_dispatcher_init(SDSPIApp* app) {
    FURI_LOG_T(TAG, "app_view_dispatcher_init");
    app->view_dispatcher = view_dispatcher_alloc();
    

    // allocate each view
    FURI_LOG_D(TAG, "app_view_dispatcher_init allocating views");
    app->menu = submenu_alloc();
    app->tb_status = text_box_alloc();
    app->text_input = text_input_alloc();
    app->widget_about = widget_alloc();
    app->dialog = dialog_ex_alloc();

    app->input_pwd = "";

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path;
    path = furi_string_alloc();
    furi_string_set_str(path, EXT_PATH("apps_data/sdspi"));
    path_append(path,STORAGE_LOCKED_FILE);
    if(storage_file_exists(storage,furi_string_get_cstr(path))) {
      File* file = storage_file_alloc(storage);
      if(storage_file_open(file, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
          FURI_LOG_E(TAG, "File pwd loading");
          char data[PASSWORD_MAX_LEN] = {0};
          if(storage_file_read(file, data, PASSWORD_MAX_LEN)>0){
            FURI_LOG_E(TAG, "File pwd laoded");
            // app->input_pwd = data;
            strncpy(app->input_pwd,data,PASSWORD_MAX_LEN);
            FURI_LOG_E(TAG, data);
          }
          storage_file_close(file);
      }
      else{
        FURI_LOG_E(TAG, "File pwd not found");
      }
      FURI_LOG_E(TAG, "storage_file_free");
      storage_file_free(file);
    }
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);

    FURI_LOG_D(TAG, "app_view_dispatcher_init setting callbacks");
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, app_scene_manager_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, app_scene_manager_navigation_event_callback);

    // add views to the dispatcher, indexed by their enum value
    FURI_LOG_D(TAG, "app_view_dispatcher_init adding view menu");
    view_dispatcher_add_view(app->view_dispatcher, AppView_Menu, submenu_get_view(app->menu));

    FURI_LOG_D(TAG, "app_view_dispatcher_init adding view textbox");
    view_dispatcher_add_view(app->view_dispatcher, AppView_Status, text_box_get_view(app->tb_status));

    FURI_LOG_D(TAG, "app_view_dispatcher_init adding view dialog");
    view_dispatcher_add_view(app->view_dispatcher, AppView_Dialog, dialog_ex_get_view(app->dialog));

    FURI_LOG_D(TAG, "app_view_dispatcher_init adding view password");
    view_dispatcher_add_view(app->view_dispatcher, AppView_TextInput, text_input_get_view(app->text_input));

    FURI_LOG_D(TAG, "app_view_dispatcher_init adding view about");
    view_dispatcher_add_view(app->view_dispatcher, AppView_Info, widget_get_view(app->widget_about));
}

/** initialise app data, scene manager, and view dispatcher */
SDSPIApp* app_init() {
    FURI_LOG_T(TAG, "app_init");
    SDSPIApp* app = malloc(sizeof(SDSPIApp));
    app_scene_manager_init(app);
    app_view_dispatcher_init(app);
    return app;
}

/** free all app data, scene manager, and view dispatcher */
void app_free(SDSPIApp* app) {
    FURI_LOG_T(TAG, "app_free");
    scene_manager_free(app->scene_manager);
    view_dispatcher_remove_view(app->view_dispatcher, AppView_Menu);
    view_dispatcher_remove_view(app->view_dispatcher, AppView_Status);
    view_dispatcher_remove_view(app->view_dispatcher, AppView_TextInput);
    view_dispatcher_remove_view(app->view_dispatcher, AppView_Dialog);
    view_dispatcher_remove_view(app->view_dispatcher, AppView_Info);
    view_dispatcher_free(app->view_dispatcher);
    submenu_free(app->menu);
    text_box_free(app->tb_status);
    widget_free(app->widget_about);
    dialog_ex_free(app->dialog);
    text_input_free(app->text_input);
    free(app);
}

/** go to trace log level in the dev environment */
void app_set_log_level() {
#ifdef FURI_DEBUG
    furi_log_set_level(FuriLogLevelTrace);
#else
    furi_log_set_level(FuriLogLevelInfo);
#endif
}

/** entrypoint */
int32_t sd_spi_app(void* p) {
    UNUSED(p);
    app_set_log_level();

    // create the app context struct, scene manager, and view dispatcher
    FURI_LOG_I(TAG, "sd_spi_app starting...");
    SDSPIApp* app = app_init();

    // set the scene and launch the main loop
    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, AppScene_MainMenu);
    FURI_LOG_D(TAG, "Starting dispatcher...");
    view_dispatcher_run(app->view_dispatcher);

    // free all memory
    FURI_LOG_I(TAG, "app finishing...");
    furi_record_close(RECORD_GUI);
    app_free(app);
    return 0;
}
