#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} == --help ]]; then
	printf 'Usage: %s\nBuilds and uploads the newest plugin tag for the newest DFHack release.\n' "$0"
	exit
fi

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
output_dir=${OUTPUT_DIR:-"$repo_dir/dist"}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/xpredux.XXXXXX")
trap 'rm -rf -- "$work_dir"' EXIT

for tool in git gh cmake ninja zip unzip docker perl; do
	command -v "$tool" >/dev/null || { echo "Missing '$tool'." >&2; exit 1; }
done
gh auth status >/dev/null

if [[ -z ${CC:-} || -z ${CXX:-} ]]; then
	command -v gcc-15 >/dev/null && command -v g++-15 >/dev/null || {
		echo 'DFHack requires GCC 15 or older; install gcc-15 and g++-15, or set CC and CXX.' >&2
		exit 1
	}
	export CC=${CC:-gcc-15}
	export CXX=${CXX:-g++-15}
fi

git -C "$repo_dir" fetch --tags origin
plugin_tag=$(git -C "$repo_dir" tag --sort=-version:refname | head -n1)
[[ -n "$plugin_tag" ]] || { echo 'No plugin tag found.' >&2; exit 1; }
dfhack_tag=$(gh api repos/DFHack/dfhack/releases/latest --jq .tag_name)
repo=$(cd "$repo_dir" && gh repo view --json nameWithOwner --jq .nameWithOwner)
release_tag="$plugin_tag-dfhack-$dfhack_tag"
win_jobs=${DFHACK_JOBS:-2}
[[ "$win_jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'DFHACK_JOBS must be a positive integer.' >&2; exit 1; }

git clone --depth 1 --branch "$dfhack_tag" --recurse-submodules \
	https://github.com/DFHack/dfhack.git "$work_dir/dfhack"
mkdir -p "$work_dir/dfhack/plugins/external/xpredux"
git -C "$repo_dir" archive "$plugin_tag" CMakeLists.txt xpredux.cpp \
	movement_feature.cpp movement_feature.h camera_feature.cpp camera_feature.h \
	sprite_flip_feature.cpp sprite_flip_feature.h feature_context.h visual_animation.h \
	test_visual_animation_manager.cpp test_camera_feature.cpp test_sprite_flip_feature.cpp | \
	tar -x -C "$work_dir/dfhack/plugins/external/xpredux"

cmake -S "$work_dir/dfhack" -B "$work_dir/dfhack/build/linux" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF
cmake --build "$work_dir/dfhack/build/linux" --target xpredux
# ponytail: 2 jobs avoids exhausting local RAM; raise DFHACK_JOBS when memory permits.
sed -i "s/^jobs=.*/jobs=$win_jobs/" "$work_dir/dfhack/build/build-win64-from-linux.sh"
(cd "$work_dir/dfhack/build" && ./build-win64-from-linux.sh)

mkdir -p "$output_dir" "$work_dir/release/linux/hack/plugins" "$work_dir/release/windows/hack/plugins"
install -m755 "$work_dir/dfhack/build/linux/plugins/external/xpredux/xpredux.plug.so" "$work_dir/release/linux/hack/plugins/"
install -m755 "$work_dir/dfhack/build/win64-cross/output/hack/plugins/xpredux.plug.dll" "$work_dir/release/windows/hack/plugins/"
(cd "$work_dir/release/linux" && zip -q -r "$output_dir/xpredux-$release_tag-linux-x86_64.zip" hack)
(cd "$work_dir/release/windows" && zip -q -r "$output_dir/xpredux-$release_tag-windows-x86_64.zip" hack)
for archive in "$output_dir"/xpredux-"$release_tag"-*.zip; do unzip -t "$archive" >/dev/null; done

gh release view "$plugin_tag" --repo "$repo" >/dev/null 2>&1 || \
	gh release create "$plugin_tag" --repo "$repo" --generate-notes --title "$plugin_tag"
gh release upload "$plugin_tag" --repo "$repo" \
	"$output_dir/xpredux-$release_tag"-*.zip --clobber
printf 'Uploaded %s for DFHack %s.\n' "$plugin_tag" "$dfhack_tag"
