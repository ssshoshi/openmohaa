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
// physics.txt: which of the map's objects are physics bodies and how heavy,
// where the built-in guesses have it wrong. Read by both modules at every map
// load, written by the in-game editor (cg_physics_edit.cpp); a person can edit
// it too, and one shipped in a pk3 is read like any other file.
//
// A rule is a line: an optional map, what it is about, and settings.
//
//   model static/square_table.tik fixed      every one of them, on every map
//   m3l1b model static/chair.tik moves       every one of them on m3l1b
//   m3l1b static 57 moves mass 12            the map's 57th static model
//   m3l1b furniture 712 fixed                the brush furniture with brush 712
//   m3l1b furniture 1404 moves               brush 1404's group moves, even if its
//                                            shape was not taken for furniture
//   m3l1b entity *185 fixed                  the brush entity with model *185
//   class func_barrel fixed                  every entity of a class
//
// Settings: moves (a body that moves), fixed (solid, never moves), off (left
// out of the physics altogether), mass <kilograms>. Lines starting with # or //
// are comments. The most particular rule wins: one about a single object on a
// map, then the map's rule for its model, then the rule for its model
// everywhere, then its class.

#pragma once

#include <string>
#include <vector>

enum {
    PHYS_RULE_UNSET = -1,
    PHYS_RULE_FIXED = 0,
    PHYS_RULE_MOVES = 1,
    PHYS_RULE_OFF   = 2,
};

typedef struct {
    int   state; // PHYS_RULE_*
    float mass;  // kilograms, 0 unset
} physRule_t;

// What decided an object, for the editor to say.
typedef struct {
    physRule_t  rule;
    const char *from; // NULL, or which rule set the state ("a rule for this one"...); a string that lasts
} physRuleResult_t;

class PhysRules
{
public:
    // Replaces what is known with a file's text.
    void Parse(const char *text);
    // The file's text, comments and all.
    std::string Write() const;

    // One rule, exactly as written: map "" for every map.
    physRule_t Get(const char *map, const char *kind, const char *key) const;
    // Sets one setting of a rule, making the rule if there is none; a rule
    // left with neither (PHYS_RULE_UNSET, mass 0) goes.
    void SetState(const char *map, const char *kind, const char *key, int state);
    void SetMass(const char *map, const char *kind, const char *key, float mass);
    // Takes a rule out; whether there was one.
    bool Forget(const char *map, const char *kind, const char *key);
    // How many rules there are.
    int Count() const;
    // The keys of a map's own rules of a kind that set a state (not the rules
    // for every map): the furniture a map's rules force in, say.
    std::vector<std::string> Keys(const char *map, const char *kind, int state) const;

    // The settings for an object, the most particular rule first: its own on
    // the map (kind and key, may be NULL), its model on the map and anywhere,
    // its class on the map and anywhere (may be NULL).
    physRuleResult_t Resolve(const char *map, const char *kind, const char *key, const char *model, const char *classname) const;

    bool Empty() const { return lines.empty(); }

    // What the file starts with when the editor makes it.
    static const char *Header();

private:
    struct Line {
        std::string text; // a comment, or empty for a rule
        std::string map, kind, key;
        std::string tail; // a comment after a rule
        physRule_t  rule;
    };

    std::vector<Line> lines;

    int  Find(const char *map, const char *kind, const char *key) const;
    int  Make(const char *map, const char *kind, const char *key);
    void Tidy(int i);
};

// A model's name as rules know it: lower case, without "models/", with no
// doubled slashes ("static//Chair.tik" and "models/static/chair.tik" are both
// "static/chair.tik").
std::string Phys_RuleModel(const char *model);

// A map's name as rules know it: "maps/m3l1b.bsp" is "m3l1b".
std::string Phys_RuleMap(const char *map);
