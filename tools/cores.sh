#!/bin/bash
# The console cores Super MemCard builds against: MiSTer-devel's repos, as submodules in cores/,
# each pinned to an upstream commit. Our changes are patches (patches/), applied at build time,
# so taking a new upstream version is moving a pin, never a merge inside a fork.
#
#   tools/cores.sh list               the cores, their pins and upstream branches
#   tools/cores.sh status             which cores upstream has moved past our pin
#   tools/cores.sh update [core...]   move pins to upstream (all cores if none given)
#   tools/cores.sh sys                group the cores by their sys/ framework version
#   tools/cores.sh patch <core>       apply patches/sys and patches/<core> to cores/<core>
#   tools/cores.sh pr-all             (CI) a pull request per core upstream has moved
#   tools/cores.sh matrix [core|all]  (CI) the build list as JSON
set -euo pipefail
cd "$(dirname "$0")/.."

cores() { grep -v '^#' cores/cores.tsv | cut -f1; }
field() { awk -F'\t' -v c="$1" -v f="$2" '$1 == c { print $f }' cores/cores.tsv; }
url() { git config -f .gitmodules "submodule.cores/$1.url"; }
branch() { git config -f .gitmodules "submodule.cores/$1.branch"; }
pin() { git ls-files -s "cores/$1" | awk '{ print $2 }'; }
upstream() { git ls-remote "$(url "$1")" "refs/heads/$(branch "$1")" | cut -f1; }
known() { cores | grep -qx "$1" || { echo "no core called $1 (see cores/cores.tsv)" >&2; exit 1; }; }
repo_web() { url "$1" | sed 's/\.git$//'; }

case "${1:-status}" in
list)
    for c in $(cores); do printf '%-13s %s  %-6s %s\n' "$c" "$(pin "$c" | cut -c1-10)" "$(branch "$c")" "$(field "$c" 3)"; done ;;
status)
    moved=0
    for c in $(cores); do
        p=$(pin "$c") u=$(upstream "$c")
        if [ "$p" = "$u" ]; then printf '%-13s up to date\n' "$c"
        else printf '%-13s upstream moved: %s/compare/%s...%s\n' "$c" "$(repo_web "$c")" "${p:0:10}" "${u:0:10}"; moved=1; fi
    done
    exit $moved ;;
update)
    shift
    for c in ${@:-$(cores)}; do
        known "$c"
        u=$(upstream "$c")
        [ "$(pin "$c")" = "$u" ] && continue
        if [ -e "cores/$c/.git" ]; then
            git -C "cores/$c" fetch -q --depth 1 origin "$u" && git -C "cores/$c" checkout -q "$u"
            git add "cores/$c"
        else
            git update-index --cacheinfo "160000,$u,cores/$c"
        fi
        echo "$c -> ${u:0:10}"
    done ;;
sys)
    for c in $(cores); do
        [ -f "cores/$c/sys/hps_io.sv" ] || { echo "?          $c (not checked out)"; continue; }
        printf '%s %s\n' "$(cat cores/$c/sys/*.sv cores/$c/sys/*.v cores/$c/sys/*.vhd 2>/dev/null | sha1sum | cut -c1-10)" "$c"
    done | sort | awk '{ if ($1 != last) printf "\n%s:", $1; printf " %s", $2; last = $1 } END { print "" }' ;;
patch)
    known "$2"
    for p in patches/sys/*.patch; do [ -e "$p" ] && git -C "cores/$2" apply --directory=sys -v "../../$p"; done
    for p in patches/"$2"/*.patch; do [ -e "$p" ] && git -C "cores/$2" apply -v "../../$p"; done
    tools/sys-patch.sh "cores/$2"   # phase 2: the front-port card driver in sys_top (core/sys)
    true ;;
pr-all)
    git fetch -q origin
    for c in $(cores); do
        p=$(pin "$c") u=$(upstream "$c") b="upstream/$c"
        [ "$p" = "$u" ] && continue
        # already proposed at this commit?
        [ "$(git ls-tree "origin/$b" "cores/$c" 2>/dev/null | awk '{ print $3 }')" = "$u" ] && continue
        git checkout -q -B "$b" origin/main
        git update-index --cacheinfo "160000,$u,cores/$c"
        git commit -q -m "cores: $c upstream ${u:0:10}" -m "$(repo_web "$c")/compare/$p...$u"
        git push -q -f origin "$b"
        body="Upstream $(repo_web "$c") moved: $(repo_web "$c")/compare/$p...$u

The build of $c with our patches runs on this branch (Actions → Build cores)."
        if ! gh pr view "$b" --json state -q .state 2>/dev/null | grep -q OPEN; then
            gh pr create --base main --head "$b" --title "cores: $c upstream update" --body "$body" ||
                echo "::warning::couldn't open a PR for $c: allow Actions to create pull requests (Settings → Actions → General)"
        fi
        gh workflow run build-cores.yml --ref "$b" -f core="$c" || true
        git checkout -q main
    done ;;
matrix)
    want=${2:-all}
    [ "$want" = all ] || known "$want"
    for c in $(cores); do
        [ "$want" = all ] || [ "$want" = "$c" ] || continue
        printf '{"core":"%s","project":"%s"}\n' "$c" "$(field "$c" 2)"
    done | paste -sd, | sed 's/^/{"include":[/; s/$/]}/' ;;
*)
    sed -n '2,15p' "$0"; exit 2 ;;
esac
