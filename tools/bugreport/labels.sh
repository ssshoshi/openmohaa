#!/usr/bin/env bash
# Creates (or updates) the labels upload.py and the triage workflow use.
#   tools/bugreport/labels.sh [owner/repo]
set -euo pipefail
repo="${1:-ssshoshi/openmohaa}"

label() {
    gh label create "$1" -R "$repo" --color "$2" --description "$3" --force >/dev/null
    echo "  $1"
}

echo "Labels on $repo:"
label bug              d73a4a "Something is broken"
label idea             a2eeef "A new feature or change"
label feedback         c5def5 "An impression or opinion"

label area:physics     1d76db "Props, ragdolls, collision"
label area:texture     1d76db "Textures, shaders, sprites"
label area:lighting    1d76db "Lightmaps, light grid, shadows, dynamic lights"
label area:sound       1d76db "Sounds and music"
label area:model       1d76db "Models, animation, skins"
label area:script      1d76db "Level scripts and AI"
label area:gameplay    1d76db "Weapons, movement, rules"
label area:performance 1d76db "Frame rate, hitches, load times"
label area:other       1d76db "Anything else"

label source:ingame    5319e7 "Filed from the game (tools/bugreport/upload.py)"
label source:public    5319e7 "Filed through the public relay"
label needs-review     fbca04 "Untrusted; look before acting on it"

label agent:fixable    0e8a16 "Triage thinks the cause is clear"
label agent:fix        0e8a16 "Ask the agent for a draft fix PR"
label agent:working    bfd4f2 "The local agent is on it right now"
label agent:pr-open    0e8a16 "The agent opened a draft fix PR"
label agent:attempted  e4e669 "The agent tried a fix and did not open a PR"
label triaged          c2e0c6 "The agent has triaged this report"
label duplicate        cfd3d7 "Already reported"
