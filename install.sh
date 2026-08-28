#!/usr/bin/env bash
set -euo pipefail

df_dir=${DF_DIR:-"${HOME}/.local/share/Steam/steamapps/common/DFHack"}
plugin_src=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

for tool in git cmake ninja perl; do
	command -v "$tool" >/dev/null ||
		{
		echo "Missing '$tool'. On Arch/Omarchy: sudo pacman -S --needed base-devel cmake ninja git perl" >&2
		exit 1
		}
done

if [[ -z ${CC:-} || -z ${CXX:-} ]]; then
	if ! command -v gcc-15 >/dev/null || ! command -v g++-15 >/dev/null; then
		echo "DFHack 53.16-r1 requires GCC 15 or older. Install gcc15 or set CC and CXX." >&2
		exit 1
	fi
	export CC=${CC:-gcc-15}
	export CXX=${CXX:-g++-15}
fi

if ! perl -MXML::LibXML -MXML::LibXSLT -e 1 2>/dev/null; then
	echo "Missing DFHack Perl modules. Install them with:" >&2
	echo "sudo pacman -S --needed perl-xml-libxml perl-xml-libxslt" >&2
	exit 1
fi

if [[ ! -d "$df_dir/hack/plugins" ]]; then
	echo "DFHack plugin directory not found: $df_dir/hack/plugins" >&2
	echo "Set DF_DIR to your DFHack installation directory." >&2
	exit 1
fi

dfhack_version=${DFHACK_VERSION:-$(sed -nE '1s/^DFHack ([[:alnum:].-]+).*/\1/p' "$df_dir/hack/news.rst")}
if [[ -z "$dfhack_version" ]]; then
	echo "Could not determine the DFHack version from $df_dir/hack/news.rst." >&2
	exit 1
fi

dfhack_src=${DFHACK_SRC:-"${HOME}/Projects/dfhack-${dfhack_version}"}
dfhack_build=${DFHACK_BUILD:-"${HOME}/Projects/dfhack-build-${dfhack_version}"}

if ! git -C "$dfhack_src" rev-parse --git-dir >/dev/null 2>&1; then
	git clone --recursive --branch "$dfhack_version" \
		https://github.com/DFHack/dfhack.git "$dfhack_src"
fi

actual_version=$(git -C "$dfhack_src" describe --tags --exact-match 2>/dev/null || true)
if [[ "$actual_version" != "$dfhack_version" ]]; then
	echo "$dfhack_src is '$actual_version', expected '$dfhack_version'." >&2
	exit 1
fi
git -C "$dfhack_src" submodule update --init --recursive

external_cmake="$dfhack_src/plugins/external/CMakeLists.txt"
mkdir -p "$(dirname -- "$external_cmake")"
[[ -f "$external_cmake" ]] || touch "$external_cmake"
plugin_subdir="add_subdirectory(\"$plugin_src\" stratum-external)"
if ! grep -Fxq "$plugin_subdir" "$external_cmake"; then
	sed -i '\|add_subdirectory(df-smooth-movement)|d; \|add_subdirectory(".*/df-smooth-movement" df-smooth-movement)|d; \|add_subdirectory(".*/df-smooth-movement" smooth-movement-external)|d; \|add_subdirectory(".*/df-smooth-movement" xpredux-external)|d; \|add_subdirectory(".*/df-smooth-movement" stratum-external)|d' "$external_cmake"
	printf '\n%s\n' "$plugin_subdir" >> "$external_cmake"
fi

if [[ ! -f "$dfhack_build/build.ninja" ]]; then
	cmake --fresh -S "$dfhack_src" -B "$dfhack_build" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DDFHACK_BUILD_ARCH=64 \
		-DCMAKE_CXX_FLAGS=-Wno-error=array-bounds \
		-DCMAKE_INSTALL_PREFIX="$df_dir"
fi
if [[ "${SKIP_TESTS:-0}" == 1 ]]; then
	cmake --build "$dfhack_build" --target stratum -j2
else
	cmake --build "$dfhack_build" --target stratum stratum-animation-test stratum-camera-test stratum-sprite-flip-test -j2
	"$dfhack_build/plugins/external/stratum-external/stratum-animation-test"
	"$dfhack_build/plugins/external/stratum-external/stratum-camera-test"
	"$dfhack_build/plugins/external/stratum-external/stratum-sprite-flip-test"
fi

plugin_file="$dfhack_build/plugins/external/stratum-external/stratum.plug.so"
if [[ ! -f "$plugin_file" ]]; then
	echo "Built plugin not found under $dfhack_build." >&2
	exit 1
fi

install -m755 "$plugin_file" "$df_dir/hack/plugins/stratum.plug.so"
echo "Installed: $df_dir/hack/plugins/stratum.plug.so"
