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
// physics.txt, read and written. See phys_rules.h.

#include "phys_rules.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

static const char *pr_kinds[] = {"model", "static", "furniture", "entity", "class", NULL};

static bool PR_IsKind(const std::string& word)
{
    for (int i = 0; pr_kinds[i]; i++) {
        if (word == pr_kinds[i]) {
            return true;
        }
    }
    return false;
}

static std::string PR_Lower(const char *s)
{
    std::string out = s ? s : "";

    for (size_t i = 0; i < out.size(); i++) {
        out[i] = (char)tolower((unsigned char)out[i]);
    }
    return out;
}

std::string Phys_RuleModel(const char *model)
{
    std::string m = PR_Lower(model), out;

    for (size_t i = 0; i < m.size(); i++) {
        const char c = m[i] == '\\' ? '/' : m[i];

        if (c == '/' && !out.empty() && out[out.size() - 1] == '/') {
            continue;
        }
        out += c;
    }
    if (out.compare(0, 7, "models/") == 0) {
        out = out.substr(7);
    }
    return out;
}

std::string Phys_RuleMap(const char *map)
{
    std::string m = PR_Lower(map);
    size_t      slash = m.find_last_of("/\\");

    if (slash != std::string::npos) {
        m = m.substr(slash + 1);
    }
    if (m.size() > 4 && m.compare(m.size() - 4, 4, ".bsp") == 0) {
        m = m.substr(0, m.size() - 4);
    }
    return m;
}

void PhysRules::Parse(const char *text)
{
    std::istringstream in(text ? text : "");
    std::string        raw;

    lines.clear();

    while (std::getline(in, raw)) {
        std::istringstream       words(raw);
        std::vector<std::string> w;
        std::string              word;
        Line                     line;
        size_t                   at;

        if (!raw.empty() && raw[raw.size() - 1] == '\r') {
            raw.erase(raw.size() - 1);
        }

        while (words >> word) {
            if (word[0] == '#' || word.compare(0, 2, "//") == 0) {
                const std::streamoff end = words.tellg();

                line.tail = raw.substr((end < 0 ? raw.size() : (size_t)end) - word.size());
                break;
            }
            w.push_back(word);
        }

        line.rule.state = PHYS_RULE_UNSET;
        line.rule.mass  = 0.0f;

        at = 0;
        if (w.size() >= 2 && !PR_IsKind(PR_Lower(w[0].c_str())) && PR_IsKind(PR_Lower(w[1].c_str()))) {
            line.map = Phys_RuleMap(w[0].c_str());
            at       = 1;
        }

        if (w.size() < at + 3 || !PR_IsKind(PR_Lower(w[at].c_str()))) {
            // A comment, a blank, or something not understood: kept as it was.
            line.text = raw.empty() ? " " : raw;
            lines.push_back(line);
            continue;
        }

        line.kind = PR_Lower(w[at].c_str());
        line.key  = line.kind == "model" ? Phys_RuleModel(w[at + 1].c_str()) : PR_Lower(w[at + 1].c_str());

        for (size_t i = at + 2; i < w.size(); i++) {
            const std::string s = PR_Lower(w[i].c_str());

            if (s == "moves") {
                line.rule.state = PHYS_RULE_MOVES;
            } else if (s == "fixed") {
                line.rule.state = PHYS_RULE_FIXED;
            } else if (s == "off") {
                line.rule.state = PHYS_RULE_OFF;
            } else if (s == "mass" && i + 1 < w.size()) {
                line.rule.mass = (float)atof(w[++i].c_str());
            }
        }

        lines.push_back(line);
    }
}

std::string PhysRules::Write() const
{
    std::string out;

    for (size_t i = 0; i < lines.size(); i++) {
        const Line& l = lines[i];
        char        mass[32];

        if (!l.text.empty()) {
            out += l.text == " " ? "" : l.text;
            out += "\n";
            continue;
        }

        if (!l.map.empty()) {
            out += l.map + " ";
        }
        out += l.kind + " " + l.key;
        if (l.rule.state == PHYS_RULE_MOVES) {
            out += " moves";
        } else if (l.rule.state == PHYS_RULE_FIXED) {
            out += " fixed";
        } else if (l.rule.state == PHYS_RULE_OFF) {
            out += " off";
        }
        if (l.rule.mass > 0.0f) {
            snprintf(mass, sizeof(mass), " mass %g", l.rule.mass);
            out += mass;
        }
        if (!l.tail.empty()) {
            out += " " + l.tail;
        }
        out += "\n";
    }

    return out;
}

int PhysRules::Find(const char *map, const char *kind, const char *key) const
{
    const std::string m = map && map[0] ? Phys_RuleMap(map) : "";
    const std::string k = PR_Lower(kind);
    const std::string y = k == "model" ? Phys_RuleModel(key) : PR_Lower(key);

    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].text.empty() && lines[i].map == m && lines[i].kind == k && lines[i].key == y) {
            return (int)i;
        }
    }
    return -1;
}

physRule_t PhysRules::Get(const char *map, const char *kind, const char *key) const
{
    const int  i = Find(map, kind, key);
    physRule_t none;

    if (i >= 0) {
        return lines[i].rule;
    }

    none.state = PHYS_RULE_UNSET;
    none.mass  = 0.0f;
    return none;
}

int PhysRules::Make(const char *map, const char *kind, const char *key)
{
    const int i = Find(map, kind, key);
    Line      line;

    if (i >= 0) {
        return i;
    }

    line.map        = map && map[0] ? Phys_RuleMap(map) : "";
    line.kind       = PR_Lower(kind);
    line.key        = line.kind == "model" ? Phys_RuleModel(key) : PR_Lower(key);
    line.rule.state = PHYS_RULE_UNSET;
    line.rule.mass  = 0.0f;
    lines.push_back(line);
    return (int)lines.size() - 1;
}

void PhysRules::Tidy(int i)
{
    if (lines[i].rule.state == PHYS_RULE_UNSET && lines[i].rule.mass <= 0.0f) {
        lines.erase(lines.begin() + i);
    }
}

void PhysRules::SetState(const char *map, const char *kind, const char *key, int state)
{
    const int i = Make(map, kind, key);

    lines[i].rule.state = state;
    Tidy(i);
}

void PhysRules::SetMass(const char *map, const char *kind, const char *key, float mass)
{
    const int i = Make(map, kind, key);

    lines[i].rule.mass = mass > 0.0f ? mass : 0.0f;
    Tidy(i);
}

bool PhysRules::Forget(const char *map, const char *kind, const char *key)
{
    const int i = Find(map, kind, key);

    if (i < 0) {
        return false;
    }
    lines.erase(lines.begin() + i);
    return true;
}

std::vector<std::string> PhysRules::Keys(const char *map, const char *kind, int state) const
{
    const std::string        m = map && map[0] ? Phys_RuleMap(map) : "";
    const std::string        k = PR_Lower(kind);
    std::vector<std::string> out;

    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].text.empty() && lines[i].map == m && lines[i].kind == k && lines[i].rule.state == state) {
            out.push_back(lines[i].key);
        }
    }
    return out;
}

int PhysRules::Count() const
{
    int n = 0;

    for (size_t i = 0; i < lines.size(); i++) {
        n += lines[i].text.empty() ? 1 : 0;
    }
    return n;
}

const char *PhysRules::Header()
{
    return "// Which of the maps' objects are physics bodies, and how heavy. Written by the\n"
           "// physics editor (phys_edit in the console); it can be edited by hand too.\n"
           "//\n"
           "//   [map] model <model> <settings>     every one of a model (static/chair.tik)\n"
           "//   <map> static <number> <settings>   one of the map's static models\n"
           "//   <map> furniture <brush> <settings> a piece of furniture in the brushwork; moves\n"
           "//                                      lets in a group whose shape was turned down\n"
           "//   <map> entity <key> <settings>      an entity: *N for a brush model, else x,y,z\n"
           "//   [map] class <class> <settings>     every entity of a class (func_crate)\n"
           "//\n"
           "// Settings: moves, fixed, off (not solid at all), mass <kilograms>.\n"
           "// Without a map a rule holds on every map. The most particular rule wins.\n"
           "\n";
}

physRuleResult_t PhysRules::Resolve(const char *map, const char *kind, const char *key, const char *model, const char *classname) const
{
    physRuleResult_t result;
    struct {
        const char *map, *kind, *key;
        const char *say;
    } order[6];
    int n = 0;

    result.rule.state = PHYS_RULE_UNSET;
    result.rule.mass  = 0.0f;
    result.from       = NULL;

    if (kind && key) {
        order[n++] = {map, kind, key, "a rule for this one"};
    }
    if (model && model[0]) {
        order[n++] = {map, "model", model, "a rule for its model on this map"};
        order[n++] = {"", "model", model, "a rule for its model everywhere"};
    }
    if (classname && classname[0]) {
        order[n++] = {map, "class", classname, "a rule for its class on this map"};
        order[n++] = {"", "class", classname, "a rule for its class everywhere"};
    }

    // The state and the mass are each taken from the most particular rule
    // that has one.
    for (int i = 0; i < n; i++) {
        const physRule_t r = Get(order[i].map, order[i].kind, order[i].key);

        if (r.state != PHYS_RULE_UNSET && result.rule.state == PHYS_RULE_UNSET) {
            result.rule.state = r.state;
            result.from       = order[i].say;
        }
        if (r.mass > 0.0f && result.rule.mass <= 0.0f) {
            result.rule.mass = r.mass;
        }
    }

    return result;
}
