/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// Added in OPM
// DESCRIPTION:
// The model browser (modelbrowser, or the Models button in the cvar browser):
// every model (.tik) the game can load, filtered by name. Spawn original puts
// the selected one where the player looks, Spawn remaster its remaster
// (models/remaster/..., the pak orch.py remaster-pak builds), and Spawn both
// the two side by side, the remaster on the right. The Soldiers view lists the
// soldiers (models/human/) and spawns one with or without AI, attacking or
// not, at a set health. All go through the game's spawnmodel and spawnactor
// commands (fgame/g_orch.cpp), in single player.

#include "cl_ui.h"
#include "client.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#define MDB_REMASTER "models/remaster/"

static std::vector<std::string> mdb_models;    // every models/*.tik but the remasters, sorted
static std::set<std::string>    mdb_remastered; // the originals (lower case) that have one
static bool                     mdb_listed;

static std::string MDB_Lower(const std::string& s)
{
    std::string out(s);
    for (size_t i = 0; i < out.length(); i++) {
        out[i] = tolower((unsigned char)out[i]);
    }
    return out;
}

// One folder at a time: a single listing of models/ would pass the file
// system's 4096 file cap.
static void MDB_ListFolder(const char *folder, std::vector<std::string>& out)
{
    int    n;
    char **files = FS_ListFilteredFiles(folder, ".tik", NULL, qtrue, &n, qtrue);

    for (int i = 0; i < n; i++) {
        out.push_back(std::string(folder) + "/" + files[i]);
    }
    FS_FreeFileList(files);
}

static void MDB_List(void)
{
    std::vector<std::string> all;
    std::set<std::string>    seen;
    int                      n;
    char                   **dirs;

    if (mdb_listed) {
        return;
    }
    mdb_listed = true;

    // models/*.tik itself, then each folder with what is under it.
    {
        char **files = FS_ListFilteredFiles("models", ".tik", NULL, qfalse, &n, qtrue);
        for (int i = 0; i < n; i++) {
            all.push_back(std::string("models/") + files[i]);
        }
        FS_FreeFileList(files);
    }
    dirs = FS_ListFiles("models", "/", qfalse, &n);
    for (int i = 0; i < n; i++) {
        if (dirs[i][0] && dirs[i][0] != '.') {
            MDB_ListFolder(va("models/%s", dirs[i]), all);
        }
    }
    FS_FreeFileList(dirs);
    // A pak without folder entries (orch.py remaster-pak writes none) shows
    // no folder in that listing.
    MDB_ListFolder("models/remaster", all);

    for (size_t i = 0; i < all.size(); i++) {
        const std::string low = MDB_Lower(all[i]);

        if (!seen.insert(low).second) {
            continue;
        }
        if (!low.compare(0, strlen(MDB_REMASTER), MDB_REMASTER)) {
            mdb_remastered.insert("models/" + low.substr(strlen(MDB_REMASTER)));
        } else {
            mdb_models.push_back(all[i]);
        }
    }
}

static bool MDB_HasRemaster(const char *path)
{
    return mdb_remastered.count(MDB_Lower(path)) != 0;
}

//=============================================================
// The rows
//=============================================================

enum {
    MDB_COL_NAME,
    MDB_COL_REMASTER
};

class ModelBrowserItem : public UIListCtrlItem
{
public:
    str  path;
    bool remaster;

    ModelBrowserItem(const char *p, bool r)
        : path(p)
        , remaster(r)
    {}

    griditemtype_t getListItemType(int which) const override { return TYPE_STRING; }

    str getListItemString(int which) const override
    {
        switch (which) {
        case MDB_COL_NAME:
            return path.c_str() + 7; // without models/
        case MDB_COL_REMASTER:
            return remaster ? "remaster" : "";
        }
        return "";
    }

    int getListItemValue(int which) const override { return 0; }

    void DrawListItem(int iColumn, const UIRect2D& drawRect, bool bSelected, UIFont *pFont) override {}

    qboolean IsHeaderEntry(void) const override { return qfalse; }
};

// A list that knows which row the mouse is over.
class UIModelList : public UIListCtrl
{
public:
    CLASS_PROTOTYPE(UIModelList);

    int hoverItem;

    UIModelList()
        : hoverItem(0)
    {}

    void TrySelectItem(int which) override { UIListCtrl::TrySelectItem(which); }

    void MouseMoved(Event *ev)
    {
        const UIPoint2D p     = MouseEventToClientPoint(ev);
        const int       rowH  = m_font->getHeight(getVirtualScale());
        const int       first = m_vertscroll ? m_vertscroll->getTopItem() + 1 : 1;

        if (p.y < getHeaderHeight() || rowH <= 0) {
            hoverItem = 0;
            return;
        }
        hoverItem = first + (int)((p.y - getHeaderHeight()) / rowH);
        if (hoverItem > getNumItems()) {
            hoverItem = 0;
        }
    }

    void MouseExited(Event *ev) { hoverItem = 0; }
};

CLASS_DECLARATION(UIListCtrl, UIModelList, NULL) {
    {&W_MouseMoved,  &UIModelList::MouseMoved },
    {&W_MouseExited, &UIModelList::MouseExited},
    {NULL,           NULL                     }
};

// What the hovered or selected row is, at the bottom.
class UIModelInfo : public UIWidget
{
public:
    CLASS_PROTOTYPE(UIModelInfo);

    str    title, line1, line2;
    UIFont titleFont;

    UIModelInfo()
        : titleFont("facfont-20")
    {}

    void Draw(void) override
    {
        const float pad   = 8.0f;
        const int   lineH = m_font->getHeight(getVirtualScale());
        float       y     = pad;

        DrawBox(0, 0, m_frame.size.width, m_frame.size.height, UColor(0.01f, 0.02f, 0.015f, 1.0f), 0.95f);

        if (!title.length()) {
            m_font->setColor(ULightGrey);
            m_font->Print(pad, y, "Hover over a model; click to select it, double-click to spawn it.", -1, NULL);
            return;
        }

        titleFont.setColor(UColor(1.0f, 0.85f, 0.3f, 1.0f));
        titleFont.Print(pad, y, title.c_str(), -1, NULL);
        y += titleFont.getHeight(getVirtualScale()) + 2;

        m_font->setColor(ULightGrey);
        m_font->Print(pad, y, line1.c_str(), -1, NULL);
        y += lineH;
        m_font->setColor(UWhite);
        m_font->Print(pad, y, line2.c_str(), -1, NULL);

        m_font->setColor(UGrey);
        m_font->Print(
            pad,
            m_frame.size.height - pad - lineH,
            "Spawned where you look, facing you; they stay until Clear or a load. Single player.",
            -1,
            NULL
        );
    }
};

CLASS_DECLARATION(UIWidget, UIModelInfo, NULL) {
    {NULL, NULL}
};

//=============================================================
// The window
//=============================================================

enum {
    MDB_VIEW_ALL,
    MDB_VIEW_REMASTERED,
    MDB_VIEW_SOLDIERS,
    MDB_NUM_VIEWS
};

static const char *mdb_viewNames[] = {"all", "remastered", "soldiers"};

// A soldier: a model straight in models/human/ (all actors, but deaths.tik).
static bool MDB_IsSoldier(const char *path)
{
    return !Q_stricmpn(path, "models/human/", 13) && !strchr(path + 13, '/') && Q_stricmp(path + 13, "deaths.tik");
}

static std::map<std::string, std::vector<std::string>> mdb_weapons;

// The weapons a soldier's model has gear for: its 'case weapon "..."' lines,
// the names a map's spawner gives (and spawnactor -weapon takes).
static const std::vector<std::string>& MDB_Weapons(const char *path)
{
    const std::string key = MDB_Lower(path);
    std::map<std::string, std::vector<std::string>>::iterator it = mdb_weapons.find(key);

    if (it != mdb_weapons.end()) {
        return it->second;
    }

    std::vector<std::string>& out = mdb_weapons[key];
    void                     *buf = NULL;
    const long                len = FS_ReadFile(path, &buf);

    if (len > 0 && buf) {
        const std::string text((const char *)buf, (size_t)len);
        const std::string low = MDB_Lower(text);
        size_t            pos = 0;

        while ((pos = low.find("case weapon", pos)) != std::string::npos) {
            const size_t open  = text.find('"', pos);
            const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
            const size_t eol   = text.find('\n', pos);

            pos += 11;
            if (close == std::string::npos || (eol != std::string::npos && open > eol)) {
                continue;
            }
            out.push_back(text.substr(open + 1, close - open - 1));
        }
        FS_FreeFile(buf);
    }
    return out;
}

// The weapon a soldier would get: the last one picked, when its model has it,
// else its first that isn't "none".
static str MDB_Weapon(const char *path)
{
    const std::vector<std::string>& list   = MDB_Weapons(path);
    const char                     *picked = Cvar_VariableString("ui_mdb_weapon");

    for (size_t i = 0; i < list.size(); i++) {
        if (!Q_stricmp(list[i].c_str(), picked)) {
            return list[i].c_str();
        }
    }
    for (size_t i = 0; i < list.size(); i++) {
        if (Q_stricmp(list[i].c_str(), "none")) {
            return list[i].c_str();
        }
    }
    return "";
}

static Event W_ModelBrowser_ChildSizeChanged(
    "modelbrowser_childsizechanged", EV_DEFAULT, NULL, NULL, "Signal that the child area of the model browser changed size"
);

class UIModelBrowser;
static UIModelBrowser *mdb_window;
static str             mdb_selected; // kept between openings
static int             mdb_view;

class UIModelBrowser : public UIFloatingWindow
{
public:
    CLASS_PROTOTYPE(UIModelBrowser);

    UIModelList *list;
    UIModelInfo *info;
    UIField     *filter;
    UILabel     *filterLabel, *countLabel, *selectedLabel;
    UIButton    *views[MDB_NUM_VIEWS];
    UIButton    *spawnButton, *remasterButton, *compareButton, *clearButton, *cvarsButton;
    // The Soldiers view's own row.
    UIButton    *aiButton, *attackButton, *weaponButton, *soldierButton;
    UILabel     *healthLabel;
    UIField     *health;

    str lastFilter;

    UIModelBrowser()
        : list(NULL)
        , info(NULL)
    {}

    ~UIModelBrowser()
    {
        if (mdb_window == this) {
            mdb_window = NULL;
        }
    }

    UIButton *MakeButton(const char *title, const char *command)
    {
        UIButton *b = new UIButton();
        b->InitFrame(getChildSpace(), 0, 0, 80, 22, border_outline, "verdana-12");
        b->setTitle(title);
        b->setBackgroundColor(UColor(0.12f, 0.16f, 0.24f, 1.0f), true);
        b->setForegroundColor(UHudColor);
        b->LinkCommand(command);
        return b;
    }

    UILabel *MakeLabel(const char *text)
    {
        UILabel *l = new UILabel();
        l->InitFrame(getChildSpace(), 0, 0, 80, 22, 0, "verdana-12");
        l->SetLabel(text);
        l->setBackgroundColor(UColor(0.0f, 0.0f, 0.0f, 0.0f), true);
        l->setForegroundColor(UHudColor);
        return l;
    }

    void Build(void)
    {
        filterLabel = MakeLabel("Filter");

        filter = new UIField();
        filter->InitFrame(getChildSpace(), 0, 0, 200, 22, border_indent, "verdana-14");
        filter->setBackgroundColor(UColor(0.0f, 0.0f, 0.0f, 1.0f), true);
        filter->setForegroundColor(UWhite);
        filter->LinkCvar("ui_mdb_filter");

        views[0] = MakeButton("All", "modelbrowser_view all");
        views[1] = MakeButton("Remastered", "modelbrowser_view remastered");
        views[2] = MakeButton("Soldiers", "modelbrowser_view soldiers");
        countLabel = MakeLabel("");

        list = new UIModelList();
        list->InitFrame(getChildSpace(), 0, 0, 100, 100, 0, "verdana-14");
        list->setBackgroundColor(UColor(0.02f, 0.05f, 0.03f), true);
        list->setForegroundColor(UHudColor);
        list->setHeaderFont("verdana-12");
        list->AddColumn("Model", MDB_COL_NAME, 470, false, false);
        list->AddColumn("", MDB_COL_REMASTER, 90, false, false);
        list->Connect(this, EV_UIListBase_ItemSelected, EV_UIListBase_ItemSelected);
        list->Connect(this, EV_UIListBase_ItemDoubleClicked, EV_UIListBase_ItemDoubleClicked);
        list->AllowActivate(true);
        list->SetDontLocalize();

        info = new UIModelInfo();
        info->InitFrame(getChildSpace(), 0, 0, 100, 100, 0, "verdana-14");

        selectedLabel = MakeLabel("");
        spawnButton    = MakeButton("Spawn original", "modelbrowser_spawn");
        remasterButton = MakeButton("Spawn remaster", "modelbrowser_spawn remaster");
        compareButton  = MakeButton("Spawn both", "modelbrowser_spawn both");
        clearButton   = MakeButton("Clear", "spawnmodel -clear");
        cvarsButton   = MakeButton("Cvars", "modelbrowser; cvarbrowser");

        aiButton      = MakeButton("", "toggle ui_mdb_noai");
        attackButton  = MakeButton("", "toggle ui_mdb_noattack");
        weaponButton  = MakeButton("", "modelbrowser_weapon");
        healthLabel   = MakeLabel("Health");
        health        = new UIField();
        health->InitFrame(getChildSpace(), 0, 0, 60, 22, border_indent, "verdana-14");
        health->setBackgroundColor(UColor(0.0f, 0.0f, 0.0f, 1.0f), true);
        health->setForegroundColor(UWhite);
        health->LinkCvar("ui_mdb_health");
        soldierButton = MakeButton("Spawn soldier", "modelbrowser_spawn soldier");

        getChildSpace()->Connect(this, W_SizeChanged, W_ModelBrowser_ChildSizeChanged);
        Layout();
        Refill();
    }

    void Layout(void)
    {
        const UISize2D s     = getChildSpace()->getSize();
        const float    pad   = 6.0f;
        const float    rowH  = 22.0f;
        const float    infoH = 92.0f;
        float          x     = pad;

        filterLabel->setFrame(UIRect2D(x, pad, 40, rowH));
        x += 44;
        filter->setFrame(UIRect2D(x, pad, 200, rowH));
        x += 208;
        views[0]->setFrame(UIRect2D(x, pad, 50, rowH));
        x += 54;
        views[1]->setFrame(UIRect2D(x, pad, 90, rowH));
        x += 94;
        views[2]->setFrame(UIRect2D(x, pad, 70, rowH));
        x += 74;
        countLabel->setFrame(UIRect2D(x + 6, pad, s.width - x - 12, rowH));

        const float listTop = pad * 2 + rowH;
        const float editTop = s.height - pad - rowH;
        const float selTop  = editTop - 4 - rowH;
        const float infoTop = selTop - pad - infoH;

        list->setFrame(UIRect2D(pad, listTop, s.width - pad * 2, infoTop - pad - listTop));
        info->setFrame(UIRect2D(pad, infoTop, s.width - pad * 2, infoH));

        selectedLabel->setFrame(UIRect2D(pad, selTop, s.width - pad * 2, rowH));
        x = pad;
        spawnButton->setFrame(UIRect2D(x, editTop, 120, rowH));
        x += 124;
        remasterButton->setFrame(UIRect2D(x, editTop, 120, rowH));
        x += 124;
        compareButton->setFrame(UIRect2D(x, editTop, 120, rowH));

        // In the same place, for the Soldiers view.
        x = pad;
        aiButton->setFrame(UIRect2D(x, editTop, 74, rowH));
        x += 78;
        attackButton->setFrame(UIRect2D(x, editTop, 94, rowH));
        x += 98;
        weaponButton->setFrame(UIRect2D(x, editTop, 220, rowH));
        x += 226;
        healthLabel->setFrame(UIRect2D(x, editTop, 44, rowH));
        x += 46;
        health->setFrame(UIRect2D(x, editTop, 64, rowH));
        x += 70;
        soldierButton->setFrame(UIRect2D(x, editTop, 110, rowH));
        clearButton->setFrame(UIRect2D(s.width - pad - 148, editTop, 70, rowH));
        cvarsButton->setFrame(UIRect2D(s.width - pad - 74, editTop, 70, rowH));
    }

    void OnChildSizeChanged(Event *ev) { Layout(); }

    void Dim(UIButton *b, bool usable)
    {
        b->setForegroundColor(usable ? UHudColor : UColor(0.4f, 0.4f, 0.4f, 1.0f));
        b->setBackgroundColor(usable ? UColor(0.12f, 0.16f, 0.24f, 1.0f) : UColor(0.08f, 0.09f, 0.11f, 1.0f), true);
    }

    void Refill(void)
    {
        const str needle = Cvar_VariableString("ui_mdb_filter");
        int       count  = 0;

        MDB_List();
        list->DeleteAllItems();

        for (size_t i = 0; i < mdb_models.size(); i++) {
            const char *p        = mdb_models[i].c_str();
            const bool  remaster = MDB_HasRemaster(p);

            if (mdb_view == MDB_VIEW_REMASTERED && !remaster) {
                continue;
            }
            if (mdb_view == MDB_VIEW_SOLDIERS && !MDB_IsSoldier(p)) {
                continue;
            }
            if (needle.length() && !Q_stristr(p, needle.c_str())) {
                continue;
            }
            list->AddItem(new ModelBrowserItem(p, remaster));
            count++;
        }

        list->SortByColumn(MDB_COL_NAME);
        lastFilter = needle;
        countLabel->SetLabel(
            mdb_view == MDB_VIEW_SOLDIERS ? va("%d soldiers", count)
            : mdb_remastered.size() ? va("%d shown, %d with a remaster", count, (int)mdb_remastered.size())
                                  : va("%d shown; no remaster pak loaded", count)
        );

        // The last selection, when still listed.
        for (int i = 1; i <= list->getNumItems(); i++) {
            if (!Q_stricmp(((ModelBrowserItem *)list->GetItem(i))->path, mdb_selected)) {
                list->TrySelectItem(i);
                break;
            }
        }
    }

    void SetView(int v)
    {
        mdb_view = v;
        Refill();
    }

    ModelBrowserItem *Item(int n)
    {
        return n > 0 && n <= list->getNumItems() ? (ModelBrowserItem *)list->GetItem(n) : NULL;
    }

    // The row an event is about: forwarded signals can come without it.
    int EventItem(Event *ev) { return ev->NumArgs() > 0 ? ev->GetInteger(1) : list->getCurrentItem(); }

    void OnItemSelected(Event *ev)
    {
        ModelBrowserItem *item = Item(EventItem(ev));

        if (item) {
            mdb_selected = item->path;
        }
    }

    void OnItemDoubleClicked(Event *ev)
    {
        ModelBrowserItem *item = Item(EventItem(ev));

        if (item) {
            mdb_selected = item->path;
            Cbuf_AddText(mdb_view == MDB_VIEW_SOLDIERS ? "modelbrowser_spawn soldier\n" : "modelbrowser_spawn\n");
        }
    }

    void Draw(void) override
    {
        const str needle = Cvar_VariableString("ui_mdb_filter");
        const bool remaster = mdb_selected.length() && MDB_HasRemaster(mdb_selected);

        UIFloatingWindow::Draw();

        if (needle != lastFilter) {
            Refill();
        }

        {
            int               n    = list->hoverItem ? list->hoverItem : list->getCurrentItem();
            ModelBrowserItem *item = Item(n);

            if (item) {
                const char *slash = strrchr(item->path.c_str(), '/');

                info->title = slash ? slash + 1 : item->path.c_str();
                info->line1 = item->path;
                if (mdb_view == MDB_VIEW_SOLDIERS) {
                    info->line2 = "Set AI, Attack, Weapon and Health below (Health empty: the model's own), then Spawn soldier.";
                } else {
                    info->line2 = item->remaster ? va("Has a remaster: %s", MDB_REMASTER) : "No remaster.";
                    if (item->remaster) {
                        info->line2 += str(item->path.c_str() + 7) + "   Spawn both puts it on the original's right.";
                    }
                }
            } else {
                info->title = "";
            }
        }

        for (int i = 0; i < MDB_NUM_VIEWS; i++) {
            views[i]->setBackgroundColor(
                i == mdb_view ? UColor(0.25f, 0.32f, 0.45f, 1.0f) : UColor(0.12f, 0.16f, 0.24f, 1.0f), true
            );
        }

        selectedLabel->SetLabel(mdb_selected.length() ? str("Selected: ") + mdb_selected : str("Click a model to select it"));
        // Always there; dimmed while there is nothing for them to spawn.
        Dim(spawnButton, mdb_selected.length() != 0);
        Dim(remasterButton, remaster);
        Dim(compareButton, remaster);

        const bool soldiers = mdb_view == MDB_VIEW_SOLDIERS;
        spawnButton->setShow(!soldiers);
        remasterButton->setShow(!soldiers);
        compareButton->setShow(!soldiers);
        aiButton->setShow(soldiers);
        attackButton->setShow(soldiers);
        healthLabel->setShow(soldiers);
        health->setShow(soldiers);
        soldierButton->setShow(soldiers);
        weaponButton->setShow(soldiers);
        aiButton->setTitle(Cvar_VariableIntegerValue("ui_mdb_noai") ? "AI: off" : "AI: on");
        attackButton->setTitle(Cvar_VariableIntegerValue("ui_mdb_noattack") ? "Attack: off" : "Attack: on");
        Dim(soldierButton, MDB_IsSoldier(mdb_selected));
        {
            const str w = MDB_IsSoldier(mdb_selected) ? MDB_Weapon(mdb_selected) : str();
            weaponButton->setTitle(str("Weapon: ") + (w.length() ? w : str("-")));
            Dim(weaponButton, MDB_Weapons(mdb_selected).size() > 1);
        }
    }
};

CLASS_DECLARATION(UIFloatingWindow, UIModelBrowser, NULL) {
    {&EV_UIListBase_ItemSelected,      &UIModelBrowser::OnItemSelected     },
    {&EV_UIListBase_ItemDoubleClicked, &UIModelBrowser::OnItemDoubleClicked},
    {&W_ModelBrowser_ChildSizeChanged, &UIModelBrowser::OnChildSizeChanged },
    {NULL,                             NULL                                }
};

//=============================================================
// Commands
//=============================================================

qboolean UI_ModelBrowserOpen(void)
{
    return mdb_window ? qtrue : qfalse;
}

static void MDB_Close(void)
{
    if (mdb_window) {
        mdb_window->PostEvent(EV_Remove, 0);
        mdb_window = NULL;
    }
    if (clc.state == CA_ACTIVE && !UI_MenuActive()) {
        UI_ActivateView3D();
        // The mouse back to looking around, unless the console still has it.
        if (!UI_ConsoleIsVisible()) {
            IN_MouseOff();
        }
    }
}

void UI_CloseModelBrowser(void)
{
    MDB_Close();
}

// The listed model a name means: a path (models/ and .tik optional), or a
// bare file name matched in any folder; the first match.
static str MDB_Find(const char *name)
{
    std::string want = MDB_Lower(name);
    const bool  bare = want.find('/') == std::string::npos;

    if (want.length() < 4 || want.compare(want.length() - 4, 4, ".tik")) {
        want += ".tik";
    }
    if (bare) {
        want = "/" + want;
    } else if (want.compare(0, 7, "models/")) {
        want = "models/" + want;
    }

    MDB_List();
    for (size_t i = 0; i < mdb_models.size(); i++) {
        const std::string low = MDB_Lower(mdb_models[i]);

        if (bare ? low.length() > want.length() && !low.compare(low.length() - want.length(), want.length(), want)
                 : low == want) {
            return mdb_models[i].c_str();
        }
    }
    return "";
}

// modelbrowser [model]: opens the window, or closes it when open. With a
// model, opens it on that one, selected.
void UI_ModelBrowser_f(void)
{
    const char *name = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";

    if (name[0]) {
        const str found = MDB_Find(name);

        if (!found.length()) {
            Com_Printf("modelbrowser: no model %s\n", name);
            return;
        }
        mdb_selected = found;
        Cvar_Set("ui_mdb_filter", strrchr(found.c_str(), '/') + 1);
        if (mdb_window) {
            mdb_window->Refill();
            return;
        }
    } else if (mdb_window) {
        MDB_Close();
        return;
    }

    const float w = Q_min(900.0f, uid.vidWidth * 0.9f);
    const float h = Q_min(700.0f, uid.vidHeight * 0.9f);

    mdb_window = new UIModelBrowser();
    mdb_window->Create(
        NULL, UIRect2D((uid.vidWidth - w) / 2, (uid.vidHeight - h) / 2, w, h), "Models", UColor(0.1f, 0.13f, 0.19f), UHudColor
    );
    mdb_window->Build();
    uWinMan.ActivateControl(mdb_window);
    // A cursor for it, as a menu has: opened from a key in game, the mouse
    // would still turn the view.
    IN_MouseOn();
}

// modelbrowser_view <all|remastered>
static void UI_ModelBrowserView_f(void)
{
    if (!mdb_window) {
        return;
    }
    for (int i = 0; i < MDB_NUM_VIEWS; i++) {
        if (!Q_stricmp(Cmd_Argv(1), mdb_viewNames[i])) {
            mdb_window->SetView(i);
        }
    }
}

// modelbrowser_spawn [remaster|both|soldier]: the selected model where the
// player looks, its remaster instead, both (the remaster on the original's
// right), or as a soldier with the Soldiers view's settings; the window closes
// so they can be seen.
static void UI_ModelBrowserSpawn_f(void)
{
    const char *which = Cmd_Argv(1);

    if (!mdb_selected.length()) {
        return;
    }
    if (!Q_stricmp(which, "soldier")) {
        const int hp = Cvar_VariableIntegerValue("ui_mdb_health");

        if (!MDB_IsSoldier(mdb_selected)) {
            Com_Printf("modelbrowser: %s isn't a soldier\n", mdb_selected.c_str());
            return;
        }
        MDB_Close();
        const str weapon = MDB_Weapon(mdb_selected);

        Cbuf_AddText(va(
            "spawnactor %s%s%s%s\"%s\"\n",
            Cvar_VariableIntegerValue("ui_mdb_noai") ? "-noai " : "",
            Cvar_VariableIntegerValue("ui_mdb_noattack") ? "-noattack " : "",
            hp > 0 ? va("-health %d ", hp) : "",
            weapon.length() ? va("-weapon \"%s\" ", weapon.c_str()) : "",
            mdb_selected.c_str()
        ));
        return;
    }
    if ((!Q_stricmp(which, "remaster") || !Q_stricmp(which, "both")) && !MDB_HasRemaster(mdb_selected)) {
        Com_Printf("modelbrowser: %s has no remaster\n", mdb_selected.c_str());
        return;
    }
    MDB_Close();
    if (!Q_stricmp(which, "remaster")) {
        Cbuf_AddText(va("spawnmodel \"%s%s\"\n", MDB_REMASTER, mdb_selected.c_str() + 7));
    } else {
        Cbuf_AddText(va("spawnmodel %s\"%s\"\n", !Q_stricmp(which, "both") ? "-compare " : "", mdb_selected.c_str()));
    }
}

// modelbrowser_weapon: the selected soldier's next weapon.
static void UI_ModelBrowserWeapon_f(void)
{
    const std::vector<std::string>& list = MDB_Weapons(mdb_selected);
    const str                       cur  = MDB_Weapon(mdb_selected);
    size_t                          i;

    if (list.empty()) {
        return;
    }
    for (i = 0; i < list.size() && Q_stricmp(list[i].c_str(), cur); i++) {}
    Cvar_Set("ui_mdb_weapon", list[(i + 1) % list.size()].c_str());
}

void UI_InitModelBrowser(void)
{
    Cvar_Get("ui_mdb_filter", "", CVAR_TEMP);
    // The Soldiers view's settings, kept: health empty or 0 is the model's own.
    Cvar_Get("ui_mdb_noai", "0", CVAR_ARCHIVE);
    Cvar_Get("ui_mdb_noattack", "0", CVAR_ARCHIVE);
    Cvar_Get("ui_mdb_health", "", CVAR_ARCHIVE);
    Cvar_Get("ui_mdb_weapon", "", CVAR_ARCHIVE);

    Cmd_AddCommand("modelbrowser", UI_ModelBrowser_f);
    Cmd_AddCommand("modelbrowser_view", UI_ModelBrowserView_f);
    Cmd_AddCommand("modelbrowser_spawn", UI_ModelBrowserSpawn_f);
    Cmd_AddCommand("modelbrowser_weapon", UI_ModelBrowserWeapon_f);
}

void UI_ShutdownModelBrowser(void)
{
    if (mdb_window) {
        delete mdb_window;
        mdb_window = NULL;
    }

    Cmd_RemoveCommand("modelbrowser");
    Cmd_RemoveCommand("modelbrowser_view");
    Cmd_RemoveCommand("modelbrowser_spawn");
    Cmd_RemoveCommand("modelbrowser_weapon");

    // The pk3s may change with the next game.
    mdb_models.clear();
    mdb_remastered.clear();
    mdb_weapons.clear();
    mdb_listed = false;
}
