/* menudemo - registers one setting of each type, to test the Mods menu. */
#include <hml/hml.h>

HML_MOD("menudemo", "Menu Demo", "1.0.0", "HML tests", "One setting of every type")

static int32_t g_flag = 1, g_count = 3, g_choice = 1;
static float g_amount = 0.5f;
static const char* const kChoices[] = {"Small", "Medium", "Large"};
static const HmlApi* api;
static HmlMod* me;

static void on_action(HmlMod* self, const HmlOption* o, void* user) {
    (void)o; (void)user;
    api->log(self, HML_LOG_INFO, "action pressed");
}

/* API v4 UI: a small panel in the game's look, bottom-left. */
static void on_ui(HmlEvent ev, const void* data, void* user) {
    const HmlUiEvent* u = (const HmlUiEvent*)data;
    float size = u->height / 28.0f, x = u->height / 40.0f, y = u->height * 0.70f;
    (void)ev; (void)user;
    api->ui_rect(me, x - 10, y - 8, u->height / 2.4f, size * 3.4f, api->ui_color(HML_UI_BACKDROP));
    api->ui_text(me, x, y, "Menu Demo", size, api->ui_color(HML_UI_WHITE), HML_ALIGN_LEFT);
    api->ui_text(me, x, y + size, g_flag ? "Switch: On" : "Switch: Off", size, api->ui_color(HML_UI_GOLD),
                 HML_ALIGN_LEFT);
    api->ui_text(me, x, y + size * 2, "Selected entry", size, api->ui_color(HML_UI_SELECTED), HML_ALIGN_LEFT);
}

void hml_mod_unload(void) {}

int hml_mod_init(const HmlApi* a, HmlMod* self) {
    api = a;
    me = self;
    if (a->version >= 4) {
        a->subscribe(self, HML_EVENT_UI, on_ui, 0);
        a->ui_toast(self, "Menu Demo loaded", "API v4 UI test", 6000);
    }
    HmlOption o;
    o = (HmlOption){HML_OPTION_LABEL, 0, "Section heading", 0, 0, 0, 0, 0, 0, 0, 0, 0};
    a->option_add(self, &o);
    o = (HmlOption)HML_OPTION_BOOL_INIT("Flag", "A switch", "Help text for the switch", &g_flag);
    a->option_add(self, &o);
    o = (HmlOption){HML_OPTION_INT, "Count", "A whole number", "1 to 10", &g_count, 1, 10, 1, 0, 0, 0, 0};
    a->option_add(self, &o);
    o = (HmlOption)HML_OPTION_FLOAT_INIT("Amount", "A slider", "0 to 1 in steps of 0.05", &g_amount, 0, 1, 0.05f);
    a->option_add(self, &o);
    o = (HmlOption){HML_OPTION_CHOICE, "Size", "A choice", 0, &g_choice, 0, 0, 1, kChoices, 3, 0, 0};
    a->option_add(self, &o);
    o = (HmlOption){HML_OPTION_ACTION, 0, "A button", "Runs an action", 0, 0, 0, 0, 0, 0, on_action, 0};
    a->option_add(self, &o);
    return HML_OK;
}
