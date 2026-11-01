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
// The cvar browser (cvarbrowser, bound to F7 by default): every cvar with its
// value, default, range and flags, and the cheat commands, each with a
// description. Hovering over a row shows its description large at the
// bottom; double-clicking one edits its value in place (Enter sets it, Escape
// leaves it; a cheat's arguments, Enter runs it). A changed value that only takes
// effect after a restart lights the Restart Video / Restart Sound button.
// The Models tab is the model browser (cl_uimodelbrowser.cpp): spawning
// props and soldiers.
//
// The descriptions and the ranges the code does not declare come from
// help/cvars.txt and help/cheats.txt (data/opm-cvarhelp, written with
// tools/cvarhelp/extract.py); the value, default, flags and a declared range
// come from the cvar itself.

#include "cl_ui.h"
#include "client.h"

#include <map>
#include <string>
#include <vector>

//=============================================================
// The descriptions
//=============================================================

typedef struct {
    std::string description;
    std::string where;    // where the code declares it, for the undescribed
    std::string min, max; // as written; empty when unknown
} cvarHelp_t;

typedef struct {
    std::string name, args, description;
} cheatHelp_t;

static std::map<std::string, cvarHelp_t> cvb_help;
static std::vector<cheatHelp_t>          cvb_cheats;
static bool                              cvb_helpLoaded;

static std::string CVB_Lower(const char *s)
{
    std::string out(s);
    for (size_t i = 0; i < out.length(); i++) {
        out[i] = tolower((unsigned char)out[i]);
    }
    return out;
}

// "\n" in the files stands for a line break.
static std::string CVB_Unescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] == '\\' && i + 1 < s.length() && s[i + 1] == 'n') {
            out += '\n';
            i++;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Tab-separated lines; # starts a comment line.
static void CVB_ReadTable(const char *path, std::vector<std::vector<std::string>>& rows)
{
    void *buf = NULL;
    long  len = FS_ReadFile(path, &buf);

    if (len <= 0 || !buf) {
        return;
    }

    const std::string text((const char *)buf, (size_t)len);
    FS_FreeFile(buf);

    size_t pos = 0;
    while (pos < text.length()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.length();
        }
        std::string line = text.substr(pos, end - pos);
        pos              = end + 1;

        if (!line.empty() && line[line.length() - 1] == '\r') {
            line.erase(line.length() - 1);
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::vector<std::string> cols;
        size_t                   start = 0;
        for (;;) {
            const size_t tab = line.find('\t', start);
            cols.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) {
                break;
            }
            start = tab + 1;
        }
        rows.push_back(cols);
    }
}

static void CVB_LoadHelp(void)
{
    std::vector<std::vector<std::string>> rows;

    if (cvb_helpLoaded) {
        return;
    }
    cvb_helpLoaded = true;

    // name  min  max  where  description
    CVB_ReadTable("help/cvars.txt", rows);
    for (size_t i = 0; i < rows.size(); i++) {
        const std::vector<std::string>& r = rows[i];
        cvarHelp_t                      h;

        if (r.size() < 5) {
            continue;
        }
        h.min         = r[1];
        h.max         = r[2];
        h.where       = r[3];
        h.description = CVB_Unescape(r[4]);
        cvb_help[CVB_Lower(r[0].c_str())] = h;
    }

    // name  arguments  description
    rows.clear();
    CVB_ReadTable("help/cheats.txt", rows);
    for (size_t i = 0; i < rows.size(); i++) {
        const std::vector<std::string>& r = rows[i];
        cheatHelp_t                     c;

        if (r.size() < 3) {
            continue;
        }
        c.name        = r[0];
        c.args        = r[1];
        c.description = CVB_Unescape(r[2]);
        cvb_cheats.push_back(c);
    }
}

static const cvarHelp_t *CVB_Help(const char *name)
{
    std::map<std::string, cvarHelp_t>::const_iterator it = cvb_help.find(CVB_Lower(name));
    return it != cvb_help.end() ? &it->second : NULL;
}

// What a changed value waits for before it takes effect.
typedef enum {
    CVB_RESTART_NONE,
    CVB_RESTART_VIDEO,
    CVB_RESTART_SOUND,
    CVB_RESTART_MAP
} cvbRestart_t;

static cvbRestart_t CVB_RestartKind(const cvar_t *cv)
{
    if (!(cv->flags & CVAR_LATCH)) {
        return CVB_RESTART_NONE;
    }
    if (!Q_stricmpn(cv->name, "r_", 2) || !Q_stricmpn(cv->name, "vid_", 4) || !Q_stricmpn(cv->name, "cl_renderer", 11)) {
        return CVB_RESTART_VIDEO;
    }
    if (!Q_stricmpn(cv->name, "s_", 2) || !Q_stricmpn(cv->name, "snd_", 4)) {
        return CVB_RESTART_SOUND;
    }
    return CVB_RESTART_MAP;
}

static str CVB_Flags(const cvar_t *cv)
{
    str out;

#define CVB_FLAG(f, s)   \
    if (cv->flags & f) { \
        if (out.length()) \
            out += " ";  \
        out += s;        \
    }
    CVB_FLAG(CVAR_ARCHIVE, "saved");
    CVB_FLAG(CVAR_LATCH, "latched");
    CVB_FLAG(CVAR_CHEAT, "cheat");
    CVB_FLAG(CVAR_ROM, "read-only");
    CVB_FLAG(CVAR_INIT, "startup");
    CVB_FLAG(CVAR_USERINFO, "user");
    CVB_FLAG(CVAR_SERVERINFO, "server");
    CVB_FLAG(CVAR_SYSTEMINFO, "system");
    CVB_FLAG(CVAR_USER_CREATED, "user-made");
#undef CVB_FLAG

    return out;
}

// A number as a range column shows it.
static str CVB_Number(float f)
{
    char buf[32];

    if (f == (int)f) {
        Com_sprintf(buf, sizeof(buf), "%d", (int)f);
    } else {
        Com_sprintf(buf, sizeof(buf), "%g", f);
    }
    return buf;
}

//=============================================================
// The rows
//=============================================================

enum {
    CVB_COL_MARK,
    CVB_COL_NAME,
    CVB_COL_VALUE,
    CVB_COL_DEFAULT,
    CVB_COL_MIN,
    CVB_COL_MAX,
    CVB_COL_FLAGS
};

class CvarBrowserItem : public UIListCtrlItem
{
public:
    str name;
    int cheat; // index in cvb_cheats + 1, or 0 for a cvar

    CvarBrowserItem(const char *n, int c)
        : name(n)
        , cheat(c)
    {}

    const cvar_t *Cvar(void) const { return cheat ? NULL : Cvar_FindVar(name); }

    str Min(void) const
    {
        const cvar_t     *cv = Cvar();
        const cvarHelp_t *h  = CVB_Help(name);

        if (cheat) {
            return "";
        }
        if (cv && cv->validate) {
            return CVB_Number(cv->min);
        }
        return h ? h->min.c_str() : "";
    }

    str Max(void) const
    {
        const cvar_t     *cv = Cvar();
        const cvarHelp_t *h  = CVB_Help(name);

        if (cheat) {
            return "";
        }
        if (cv && cv->validate) {
            return CVB_Number(cv->max);
        }
        return h ? h->max.c_str() : "";
    }

    griditemtype_t getListItemType(int which) const override
    {
        return which == CVB_COL_VALUE ? TYPE_OWNERDRAW : TYPE_STRING;
    }

    str getListItemString(int which) const override
    {
        const cvar_t *cv = Cvar();

        if (cheat) {
            switch (which) {
            case CVB_COL_MARK:
                return "C";
            case CVB_COL_NAME:
                return name;
            case CVB_COL_VALUE:
                return cvb_cheats[cheat - 1].args.c_str();
            case CVB_COL_FLAGS:
                return "cheat command";
            default:
                return "";
            }
        }

        if (!cv) {
            return "";
        }

        switch (which) {
        case CVB_COL_MARK:
            return cv->latchedString ? "L" : strcmp(cv->string, cv->resetString ? cv->resetString : "") ? "*" : "";
        case CVB_COL_NAME:
            return cv->name;
        case CVB_COL_VALUE:
            return cv->latchedString ? str(cv->string) + " -> " + cv->latchedString : str(cv->string);
        case CVB_COL_DEFAULT:
            return cv->resetString ? cv->resetString : "";
        case CVB_COL_MIN:
            return Min();
        case CVB_COL_MAX:
            return Max();
        case CVB_COL_FLAGS:
            return CVB_Flags(cv);
        }
        return "";
    }

    int getListItemValue(int which) const override { return atoi(getListItemString(which)); }

    // The value, coloured: yellow when changed from the default, orange when
    // waiting on a restart, green for a cheat's arguments.
    void DrawListItem(int iColumn, const UIRect2D& drawRect, bool bSelected, UIFont *pFont) override
    {
        const cvar_t *cv = Cvar();
        UColor        text(UHudColor);

        if (cheat) {
            text = UColor(0.5f, 0.9f, 0.5f, 1.0f);
        } else if (cv && cv->latchedString) {
            text = UColor(1.0f, 0.6f, 0.2f, 1.0f);
        } else if (cv && cv->resetString && strcmp(cv->string, cv->resetString)) {
            text = UColor(1.0f, 0.95f, 0.3f, 1.0f);
        }

        DrawBox(drawRect, bSelected ? UColor(0.21f, 0.18f, 0.015f, 1.0f) : UColor(0.02f, 0.05f, 0.03f, 1.0f), 1.0f);
        pFont->setColor(text);
        pFont->Print(drawRect.pos.x + 1.0f, drawRect.pos.y, getListItemString(iColumn).c_str(), -1, NULL);
        pFont->setColor(bSelected ? UColor(0.9f, 0.8f, 0.6f, 1.0f) : UHudColor);
    }

    qboolean IsHeaderEntry(void) const override { return qfalse; }
};

// A list that knows which row the mouse is over.
class UICvarList : public UIListCtrl
{
public:
    CLASS_PROTOTYPE(UICvarList);

    int hoverItem;

    UICvarList()
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

    // Where row n's cell of a column is drawn, in the list's own frame;
    // false when the row is scrolled out of view.
    bool CellRect(int n, int columnName, UIRect2D& out)
    {
        const int rowH  = m_font->getHeight(getVirtualScale());
        const int first = m_vertscroll ? m_vertscroll->getTopItem() + 1 : 1;
        float     x     = 0;
        float     y;

        if (n < first || n > getNumItems() || rowH <= 0) {
            return false;
        }
        y = getHeaderHeight() + (n - first) * rowH;
        if (y + rowH > m_frame.size.height) {
            return false;
        }
        for (int c = 1; c <= m_columnlist.NumObjects(); c++) {
            if (m_columnlist.ObjectAt(c).name == columnName) {
                out = UIRect2D(x, y, m_columnlist.ObjectAt(c).width, rowH);
                return true;
            }
            x += m_columnlist.ObjectAt(c).width;
        }
        return false;
    }
};

CLASS_DECLARATION(UIListCtrl, UICvarList, NULL) {
    {&W_MouseMoved,  &UICvarList::MouseMoved },
    {&W_MouseExited, &UICvarList::MouseExited},
    {NULL,           NULL                    }
};

// The large description at the bottom.
class UICvarInfo : public UIWidget
{
public:
    CLASS_PROTOTYPE(UICvarInfo);

    str title, subtitle, body, footer;

    UIFont titleFont;

    UICvarInfo()
        : titleFont("facfont-20")
    {}

    // Word-wrapped to width, from y down to bottom; returns the next y.
    float PrintWrapped(UIFont *font, const str& text, float x, float y, float width, float bottom)
    {
        const int   lineH = font->getHeight(getVirtualScale());
        const char *s     = text.c_str();

        while (*s && y + lineH <= bottom) {
            const char *eol = strchr(s, '\n');
            const int   len = eol ? (int)(eol - s) : (int)strlen(s);
            int         fit = len;

            // The most whole words of this paragraph that fit on the line.
            while (fit > 0 && font->getWidth(s, fit) > width) {
                int back = fit - 1;
                while (back > 0 && s[back] != ' ') {
                    back--;
                }
                fit = back > 0 ? back : fit - 1;
            }

            if (fit > 0) {
                font->Print(x, y, str(s, 0, fit).c_str(), -1, NULL);
            }
            y += lineH;

            if (fit >= len) {
                // The paragraph is done: past its line break, if any.
                s += len;
                if (*s == '\n') {
                    s++;
                }
            } else {
                s += Q_max(fit, 1);
                while (*s == ' ') {
                    s++;
                }
            }
        }
        return y;
    }

    void Draw(void) override
    {
        const float pad    = 8.0f;
        const float width  = m_frame.size.width - pad * 2;
        const float bottom = m_frame.size.height - pad;
        float       y      = pad;

        DrawBox(0, 0, m_frame.size.width, m_frame.size.height, UColor(0.01f, 0.02f, 0.015f, 1.0f), 0.95f);

        if (!title.length()) {
            m_font->setColor(ULightGrey);
            m_font->Print(pad, y, "Hover over a row for its description; click to select it.", -1, NULL);
            return;
        }

        titleFont.setColor(UColor(1.0f, 0.85f, 0.3f, 1.0f));
        titleFont.Print(pad, y, title.c_str(), -1, NULL);
        y += titleFont.getHeight(getVirtualScale()) + 2;

        if (subtitle.length()) {
            m_font->setColor(ULightGrey);
            y = PrintWrapped(m_font, subtitle, pad, y, width, bottom) + 4;
        }

        m_font->setColor(UWhite);
        y = PrintWrapped(m_font, body, pad, y, width, bottom - m_font->getHeight(getVirtualScale()));

        if (footer.length()) {
            m_font->setColor(UGrey);
            m_font->Print(pad, bottom - m_font->getHeight(getVirtualScale()), footer.c_str(), -1, NULL);
        }
    }
};

CLASS_DECLARATION(UIWidget, UICvarInfo, NULL) {
    {NULL, NULL}
};

//=============================================================
// The window
//=============================================================

enum {
    CVB_VIEW_ALL,
    CVB_VIEW_CHANGED,
    CVB_VIEW_LATCHED,
    CVB_VIEW_CHEATS,
    CVB_VIEW_MODELS,
    CVB_NUM_VIEWS
};

static const char *cvb_viewNames[] = {"all", "changed", "latched", "cheats", "models"};
static int         cvb_lastView; // the tab it opens on: the last one shown

// The child space resized: a window's own W_SizeChanged is taken.
static Event W_CvarBrowser_ChildSizeChanged(
    "cvarbrowser_childsizechanged", EV_DEFAULT, NULL, NULL, "Signal that the child area of the cvar browser changed size"
);

class UICvarBrowser;
static UICvarBrowser *cvb_window;

class UICvarBrowser : public UIFloatingWindow
{
public:
    CLASS_PROTOTYPE(UICvarBrowser);

    UICvarList *list;
    UICvarInfo *info;
    UIField    *filter;
    UIField    *editor; // over the Value cell of the row being edited
    UILabel    *filterLabel, *countLabel, *selectedLabel;
    UIButton   *views[CVB_NUM_VIEWS];
    UIButton   *resetButton, *runButton, *vidButton, *sndButton;
    UIWidget   *models; // the Models tab, made when first shown

    int view;
    str lastFilter;
    str selected; // the row being edited
    int selectedCheat;
    int lastCount;
    int editRow; // the list row the editor is on, 0 for none

    UICvarBrowser()
        : list(NULL)
        , info(NULL)
        , editor(NULL)
        , editRow(0)
        , models(NULL)
        , view(CVB_VIEW_ALL)
        , selectedCheat(0)
        , lastCount(-1)
    {}

    ~UICvarBrowser()
    {
        if (cvb_window == this) {
            cvb_window = NULL;
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
        // Not setBackgroundAlpha: that fades the whole widget, text too.
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
        filter->LinkCvar("ui_cvb_filter");

        for (int i = 0; i < CVB_NUM_VIEWS; i++) {
            static const char *titles[] = {"All", "Changed", "Latched", "Cheats", "Models"};
            views[i] = MakeButton(titles[i], va("cvarbrowser_view %s", cvb_viewNames[i]));
        }
        countLabel = MakeLabel("");

        list = new UICvarList();
        list->InitFrame(getChildSpace(), 0, 0, 100, 100, 0, "verdana-14");
        list->setBackgroundColor(UColor(0.02f, 0.05f, 0.03f), true);
        list->setForegroundColor(UHudColor);
        list->setHeaderFont("verdana-12");
        list->AddColumn("", CVB_COL_MARK, 16, false, false);
        list->AddColumn("Name", CVB_COL_NAME, 220, false, false);
        list->AddColumn("Value", CVB_COL_VALUE, 150, false, false);
        list->AddColumn("Default", CVB_COL_DEFAULT, 110, false, false);
        list->AddColumn("Min", CVB_COL_MIN, 56, true, false);
        list->AddColumn("Max", CVB_COL_MAX, 56, true, false);
        list->AddColumn("Flags", CVB_COL_FLAGS, 200, false, false);
        list->Connect(this, EV_UIListBase_ItemSelected, EV_UIListBase_ItemSelected);
        list->Connect(this, EV_UIListBase_ItemDoubleClicked, EV_UIListBase_ItemDoubleClicked);
        list->AllowActivate(true);
        list->SetDontLocalize();

        // The in-place editor: a child of the list, over the Value cell. Enter
        // runs cvarbrowser_set with what was typed.
        editor = new UIField();
        editor->InitFrame(list, 0, 0, 100, 20, border_indent, "verdana-14");
        editor->setBackgroundColor(UColor(0.0f, 0.0f, 0.0f, 1.0f), true);
        editor->setForegroundColor(UWhite);
        editor->LinkCvar("ui_cvb_value");
        editor->LinkCommand("cvarbrowser_set");
        editor->setShow(false);

        info = new UICvarInfo();
        info->InitFrame(getChildSpace(), 0, 0, 100, 100, 0, "verdana-14");

        selectedLabel = MakeLabel("");

        resetButton = MakeButton("Reset", "cvarbrowser_reset");
        runButton   = MakeButton("Run", "cvarbrowser_run");
        vidButton   = MakeButton("Restart Video", "vid_restart");
        sndButton   = MakeButton("Restart Sound", "snd_restart");

        getChildSpace()->Connect(this, W_SizeChanged, W_CvarBrowser_ChildSizeChanged);
        Layout();
        SetView(cvb_lastView);
    }

    void Layout(void)
    {
        const UISize2D s    = getChildSpace()->getSize();
        const float    pad  = 6.0f;
        const float    rowH = 22.0f;
        float          x    = pad;
        const float    infoH = Q_max(110.0f, s.height * 0.24f);

        // the tabs first, then what the cvar tabs filter by
        for (int i = 0; i < CVB_NUM_VIEWS; i++) {
            views[i]->setFrame(UIRect2D(x, pad, 74, rowH));
            x += 78;
        }
        x += 10;
        filterLabel->setFrame(UIRect2D(x, pad, 40, rowH));
        x += 44;
        filter->setFrame(UIRect2D(x, pad, 220, rowH));
        x += 228;
        countLabel->setFrame(UIRect2D(x + 6, pad, s.width - x - 12, rowH));

        const float listTop = pad * 2 + rowH;

        if (models) {
            models->setFrame(UIRect2D(0, listTop, s.width, s.height - listTop));
        }
        const float editTop = s.height - pad - rowH;
        const float infoTop = editTop - pad - infoH;

        list->setFrame(UIRect2D(pad, listTop, s.width - pad * 2, infoTop - pad - listTop));
        info->setFrame(UIRect2D(pad, infoTop, s.width - pad * 2, infoH));

        x = pad;
        selectedLabel->setFrame(UIRect2D(x, editTop, 420, rowH));
        x += 426;
        resetButton->setFrame(UIRect2D(x, editTop, 56, rowH));
        x += 60;
        runButton->setFrame(UIRect2D(x, editTop, 56, rowH));
        vidButton->setFrame(UIRect2D(s.width - pad - 230, editTop, 112, rowH));
        sndButton->setFrame(UIRect2D(s.width - pad - 114, editTop, 112, rowH));
    }

    void OnChildSizeChanged(Event *ev) { Layout(); }

    bool Matches(const char *name, const char *description, const str& needle)
    {
        if (!needle.length()) {
            return true;
        }
        return Q_stristr(name, needle.c_str()) || (description && Q_stristr(description, needle.c_str()));
    }

    // The rows for the view and filter, sorted by name.
    void Refill(void)
    {
        const str needle = Cvar_VariableString("ui_cvb_filter");
        int       count  = 0;

        CVB_LoadHelp();
        StopEditing();
        list->DeleteAllItems();

        if (view == CVB_VIEW_CHEATS) {
            for (size_t i = 0; i < cvb_cheats.size(); i++) {
                if (Matches(cvb_cheats[i].name.c_str(), cvb_cheats[i].description.c_str(), needle)) {
                    list->AddItem(new CvarBrowserItem(cvb_cheats[i].name.c_str(), (int)i + 1));
                    count++;
                }
            }
        } else {
            for (cvar_t *cv = Cvar_Next(NULL); cv; cv = Cvar_Next(cv)) {
                const cvarHelp_t *h = CVB_Help(cv->name);

                if (!cv->name || !cv->name[0]) {
                    continue;
                }
                if (view == CVB_VIEW_CHANGED && (!cv->resetString || !strcmp(cv->string, cv->resetString))) {
                    continue;
                }
                if (view == CVB_VIEW_LATCHED && !cv->latchedString) {
                    continue;
                }
                if (!Matches(cv->name, h ? h->description.c_str() : NULL, needle)) {
                    continue;
                }
                list->AddItem(new CvarBrowserItem(cv->name, 0));
                count++;
            }
        }

        list->SortByColumn(CVB_COL_NAME);
        lastFilter = needle;
        lastCount  = count;
        countLabel->SetLabel(va("%d shown   * changed   L waits for a restart   C cheat", count));
    }

    void SetView(int v)
    {
        const bool onModels = v == CVB_VIEW_MODELS;
        UIWidget  *cvarParts[] = {filterLabel, filter, countLabel, list, info, selectedLabel,
                                  resetButton, runButton, vidButton, sndButton};

        StopEditing();
        view         = v;
        cvb_lastView = v;
        if (onModels && !models) {
            models = UI_CreateModelPanel(getChildSpace());
            Layout();
        }
        for (size_t i = 0; i < ARRAY_LEN(cvarParts); i++) {
            cvarParts[i]->setShow(!onModels);
        }
        if (models) {
            models->setShow(onModels);
        }
        if (!onModels) {
            Refill();
        }
    }

    // Filtered to a name and that row selected, as cvarbrowser <name> asks.
    void Select(const char *name)
    {
        int v = CVB_VIEW_ALL;

        Cvar_Set("ui_cvb_filter", name);
        CVB_LoadHelp();
        for (size_t i = 0; i < cvb_cheats.size(); i++) {
            if (!Q_stricmp(cvb_cheats[i].name.c_str(), name)) {
                v = CVB_VIEW_CHEATS;
            }
        }
        SetView(v);

        for (int i = 1; i <= list->getNumItems(); i++) {
            CvarBrowserItem *item = (CvarBrowserItem *)list->GetItem(i);

            if (!Q_stricmp(item->name, name)) {
                Event ev(EV_UIListBase_ItemSelected);

                list->TrySelectItem(i);
                ev.AddInteger(i);
                OnItemSelected(&ev);
                break;
            }
        }
    }

    // Out of the editor, the keyboard back to the list.
    void StopEditing(void)
    {
        if (editor && editRow) {
            editRow = 0;
            editor->setShow(false);
            uWinMan.ActivateControl(list);
        }
    }

    // Row n's value (a cheat's arguments) to edit in place, the editor holding
    // the keyboard.
    void Edit(int n)
    {
        UIRect2D r;

        if (!list->CellRect(n, CVB_COL_VALUE, r)) {
            return;
        }
        editRow = n;
        // what it is now, or for a cheat what was typed last for it
        if (!selectedCheat) {
            const cvar_t *cv = Cvar_FindVar(selected);
            Cvar_Set("ui_cvb_value", cv ? (cv->latchedString ? cv->latchedString : cv->string) : "");
        }
        editor->UpdateData();
        editor->setFrame(UIRect2D(r.pos.x, r.pos.y - 1, Q_max(r.size.width, 200.0f), r.size.height + 2));
        editor->setShow(true);
        uWinMan.ActivateControl(editor);
    }

    void OnItemDoubleClicked(Event *ev)
    {
        const int n = ev->NumArgs() > 0 ? ev->GetInteger(1) : list->getCurrentItem();

        if (n > 0 && n <= list->getNumItems()) {
            Event sel(EV_UIListBase_ItemSelected);

            sel.AddInteger(n);
            OnItemSelected(&sel);
            Edit(n);
        }
    }

    void OnItemSelected(Event *ev)
    {
        const int        n    = ev->GetInteger(1);
        CvarBrowserItem *item = n > 0 && n <= list->getNumItems() ? (CvarBrowserItem *)list->GetItem(n) : NULL;

        if (n != editRow) {
            StopEditing();
        }
        if (!item) {
            return;
        }

        selected      = item->name;
        selectedCheat = item->cheat;
        if (item->cheat) {
            Cvar_Set("ui_cvb_value", "");
        } else {
            const cvar_t *cv = item->Cvar();
            Cvar_Set("ui_cvb_value", cv ? (cv->latchedString ? cv->latchedString : cv->string) : "");
        }
    }

    // What the panel shows for a row.
    void Describe(CvarBrowserItem *item)
    {
        if (!item) {
            info->title = "";
            return;
        }

        if (item->cheat) {
            const cheatHelp_t& c = cvb_cheats[item->cheat - 1];

            info->title    = c.name.c_str();
            info->subtitle = c.args.length() ? va("cheat command   arguments: %s", c.args.c_str()) : "cheat command, no arguments";
            info->body     = c.description.length() ? c.description.c_str() : "No description.";
            info->footer   = "Needs cheats on (thereisnomonkey 1 in single player). Double-click to type its arguments, Enter runs it; Run runs it bare.";
            return;
        }

        const cvar_t     *cv = item->Cvar();
        const cvarHelp_t *h  = CVB_Help(item->name);
        str               range;

        if (!cv) {
            info->title = "";
            return;
        }

        info->title = cv->name;
        if (item->Min().length() || item->Max().length()) {
            range = va("   range %s to %s", item->Min().length() ? item->Min().c_str() : "?", item->Max().length() ? item->Max().c_str() : "?");
        }
        info->subtitle = va(
            "value \"%s\"%s   default \"%s\"%s   %s",
            cv->string,
            cv->latchedString ? va(" (\"%s\" after a restart)", cv->latchedString) : "",
            cv->resetString ? cv->resetString : "",
            range.c_str(),
            CVB_Flags(cv).c_str()
        );

        if (h && h->description.length()) {
            info->body = h->description.c_str();
        } else if (cv->description && cv->description[0]) {
            info->body = cv->description;
        } else {
            info->body = "No description yet.";
        }

        switch (CVB_RestartKind(cv)) {
        case CVB_RESTART_VIDEO:
            info->footer = "Takes effect after Restart Video (vid_restart).";
            break;
        case CVB_RESTART_SOUND:
            info->footer = "Takes effect after Restart Sound (snd_restart).";
            break;
        case CVB_RESTART_MAP:
            info->footer = "Takes effect when the map is next loaded.";
            break;
        default:
            info->footer = h && h->where.length() ? va("Declared in %s", h->where.c_str()) : "";
            break;
        }
    }

    void Draw(void) override
    {
        const str needle  = Cvar_VariableString("ui_cvb_filter");
        bool      pendVid = false, pendSnd = false;

        UIFloatingWindow::Draw();

        for (int i = 0; i < CVB_NUM_VIEWS; i++) {
            views[i]->setBackgroundColor(
                i == view ? UColor(0.25f, 0.32f, 0.45f, 1.0f) : UColor(0.12f, 0.16f, 0.24f, 1.0f), true
            );
        }
        if (view == CVB_VIEW_MODELS) {
            return;
        }

        if (needle != lastFilter) {
            Refill();
        }

        // The editor follows its row as the list scrolls, and goes when the
        // row does; and when the keyboard went elsewhere (a click off it).
        if (editRow) {
            UIRect2D r;

            if (uWinMan.ActiveControl() != editor || !list->CellRect(editRow, CVB_COL_VALUE, r)) {
                editRow = 0;
                editor->setShow(false);
            } else {
                editor->setFrame(UIRect2D(r.pos.x, r.pos.y - 1, Q_max(r.size.width, 200.0f), r.size.height + 2));
            }
        }

        // Hovered row first, else the selected one.
        {
            int n = list->hoverItem;
            if (!n) {
                n = list->getCurrentItem();
            }
            Describe(n > 0 && n <= list->getNumItems() ? (CvarBrowserItem *)list->GetItem(n) : NULL);
        }

        for (cvar_t *cv = Cvar_Next(NULL); cv; cv = Cvar_Next(cv)) {
            if (cv->latchedString) {
                const cvbRestart_t kind = CVB_RestartKind(cv);
                pendVid |= kind == CVB_RESTART_VIDEO;
                pendSnd |= kind == CVB_RESTART_SOUND;
            }
        }

        // Lit when something waits on them.
        vidButton->setBackgroundColor(pendVid ? UColor(0.6f, 0.35f, 0.05f, 1.0f) : UColor(0.12f, 0.16f, 0.24f, 1.0f), true);
        sndButton->setBackgroundColor(pendSnd ? UColor(0.6f, 0.35f, 0.05f, 1.0f) : UColor(0.12f, 0.16f, 0.24f, 1.0f), true);

        selectedLabel->SetLabel(
            selected.length() ? str(selectedCheat ? "Cheat: " : "Selected: ") + selected + "   (double-click to edit, Enter to set)"
                              : str("Double-click a row to edit its value; Enter sets it")
        );
        runButton->setShow(selectedCheat != 0);
        resetButton->setShow(selectedCheat == 0);
    }
};

CLASS_DECLARATION(UIFloatingWindow, UICvarBrowser, NULL) {
    {&EV_UIListBase_ItemSelected,       &UICvarBrowser::OnItemSelected     },
    {&EV_UIListBase_ItemDoubleClicked,  &UICvarBrowser::OnItemDoubleClicked},
    {&W_CvarBrowser_ChildSizeChanged, &UICvarBrowser::OnChildSizeChanged},
    {NULL,                            NULL                             }
};

//=============================================================
// Commands
//=============================================================

qboolean UI_CvarBrowserOpen(void)
{
    return cvb_window ? qtrue : qfalse;
}

static void CVB_Close(void)
{
    if (cvb_window) {
        cvb_window->PostEvent(EV_Remove, 0);
        cvb_window = NULL;
    }
    // Back to the game, unless a menu is up.
    if (clc.state == CA_ACTIVE && !UI_MenuActive()) {
        UI_ActivateView3D();
        // The mouse back to looking around, unless the console still has it.
        if (!UI_ConsoleIsVisible()) {
            IN_MouseOff();
        }
    }
}

void UI_CloseCvarBrowser(void)
{
    CVB_Close();
}

static void CVB_Open(void)
{
    const float w = Q_min(1100.0f, uid.vidWidth * 0.9f);
    const float h = Q_min(760.0f, uid.vidHeight * 0.9f);

    cvb_window = new UICvarBrowser();
    cvb_window->Create(
        NULL,
        UIRect2D((uid.vidWidth - w) / 2, (uid.vidHeight - h) / 2, w, h),
        "Cvars, cheats and models (F7)",
        UColor(0.1f, 0.13f, 0.19f),
        UHudColor
    );
    cvb_window->Build();
    uWinMan.ActivateControl(cvb_window);
    // A cursor for it, as a menu has: opened from a key in game, the mouse
    // would still turn the view.
    IN_MouseOn();
}

// cvarbrowser [name]: opens the window, or closes it when open. With a name,
// opens it on that cvar or cheat.
void UI_CvarBrowser_f(void)
{
    const char *name = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";

    if (cvb_window) {
        if (name[0]) {
            cvb_window->Select(name);
        } else {
            CVB_Close();
        }
        return;
    }

    CVB_Open();
    if (name[0]) {
        cvb_window->Select(name);
    }
}

// The window open on a tab (opened if closed), as the model browser's
// commands ask.
void UI_CvarBrowserShowTab(const char *tab)
{
    if (!cvb_window) {
        CVB_Open();
    }
    for (int i = 0; cvb_window && i < CVB_NUM_VIEWS; i++) {
        if (!Q_stricmp(tab, cvb_viewNames[i]) && cvb_window->view != i) {
            cvb_window->SetView(i);
        }
    }
}

// Whether the window is open on that tab.
qboolean UI_CvarBrowserOnTab(const char *tab)
{
    return cvb_window && cvb_window->view >= 0 && cvb_window->view < CVB_NUM_VIEWS && !Q_stricmp(tab, cvb_viewNames[cvb_window->view])
             ? qtrue
             : qfalse;
}

// cvarbrowser_view <all|changed|latched|cheats|models>
static void UI_CvarBrowserView_f(void)
{
    if (!cvb_window) {
        return;
    }
    for (int i = 0; i < CVB_NUM_VIEWS; i++) {
        if (!Q_stricmp(Cmd_Argv(1), cvb_viewNames[i])) {
            cvb_window->SetView(i);
        }
    }
}

// cvarbrowser_set [value]: the selected cvar to the value, or to what was
// typed in the editor (Enter there runs this); for a cheat, it run with it.
static void UI_CvarBrowserSet_f(void)
{
    const char *v;

    if (!cvb_window || !cvb_window->selected.length()) {
        return;
    }

    v = Cmd_Argc() > 1 ? Cmd_ArgsFrom(1) : Cvar_VariableString("ui_cvb_value");
    if (cvb_window->selectedCheat) {
        Cbuf_AddText(va("%s %s\n", cvb_window->selected.c_str(), v));
    } else {
        Cvar_Set(cvb_window->selected, v);
    }
    cvb_window->StopEditing();
}

// Escape while a value is being edited leaves it, and the window open.
qboolean UI_CvarBrowserCancelEdit(void)
{
    if (!cvb_window || !cvb_window->editRow) {
        return qfalse;
    }
    cvb_window->StopEditing();
    return qtrue;
}

// cvarbrowser_reset: the selected cvar back to its default.
static void UI_CvarBrowserReset_f(void)
{
    const cvar_t *cv;

    if (!cvb_window || !cvb_window->selected.length() || cvb_window->selectedCheat) {
        return;
    }

    Cvar_Reset(cvb_window->selected);
    cv = Cvar_FindVar(cvb_window->selected);
    Cvar_Set("ui_cvb_value", cv ? (cv->latchedString ? cv->latchedString : cv->string) : "");
}

// cvarbrowser_run: the selected cheat, without arguments.
static void UI_CvarBrowserRun_f(void)
{
    if (!cvb_window || !cvb_window->selectedCheat) {
        return;
    }
    Cbuf_AddText(va("%s\n", cvb_window->selected.c_str()));
}

void UI_InitCvarBrowser(void)
{
    Cvar_Get("ui_cvb_filter", "", CVAR_TEMP);
    Cvar_Get("ui_cvb_value", "", CVAR_TEMP);

    Cmd_AddCommand("cvarbrowser", UI_CvarBrowser_f);
    Cmd_AddCommand("cvarbrowser_view", UI_CvarBrowserView_f);
    Cmd_AddCommand("cvarbrowser_set", UI_CvarBrowserSet_f);
    Cmd_AddCommand("cvarbrowser_reset", UI_CvarBrowserReset_f);
    Cmd_AddCommand("cvarbrowser_run", UI_CvarBrowserRun_f);
}

void UI_ShutdownCvarBrowser(void)
{
    if (cvb_window) {
        delete cvb_window;
        cvb_window = NULL;
    }

    Cmd_RemoveCommand("cvarbrowser");
    Cmd_RemoveCommand("cvarbrowser_view");
    Cmd_RemoveCommand("cvarbrowser_set");
    Cmd_RemoveCommand("cvarbrowser_reset");
    Cmd_RemoveCommand("cvarbrowser_run");

    // The pk3s may change with the next game.
    cvb_help.clear();
    cvb_cheats.clear();
    cvb_helpLoaded = false;
}
