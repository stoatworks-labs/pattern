#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
#     tools/verify.sh
#
# ------------------------------------------------------------------ the point
#
# Each check answers a question none of the others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out
#   demo shaders  is the browser demo's copy of the GLSL still the plugin's,
#                 character for character
#   build         a fresh universal Release build, which is what ships
#   tracker       the claims the plugin exists to make -- the cursor on the
#                 tracker's clock from a host at 499 million ms, one note per
#                 onset in the right channel on the right row, nothing false
#                 on frame one, the pitch the bin law predicts, the ring
#                 clearing and keeping, a detected tempo within a beat on a
#                 metronome AND on drum grooves at the right metrical level --
#                 measured on the tracker's published state with NO GL
#                 context anywhere near them, each with its negative control
#   grid          and the one check that does read pixels: every glyph on the
#                 whole-pixel grid at every Scale, at 640x360 and 320x180
#   pipe          the fleet's --pipe frame format writes whole frames, and a
#                 reader that hangs up gets exit 1, not SIGPIPE's silent 141
#   sweep         does every control change the picture, at two rasters
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain
#   lipo          is the macOS build really universal, or did CMake latch the
#                 architecture list before -DCMAKE_OSX_ARCHITECTURES arrived
#                 and report success anyway
#   plist         does CFBundleExecutable name the binary that is actually on
#                 disk -- if it does not, codesign reports "code object is not
#                 signed at all" about a *nested* object and mentions neither
#                 the plist nor the cause
#   codesign      the exact command the release job runs, against a copy
#   oxbow         instantiation and frames in a host, and the name, id and
#                 TYPE a host reads -- a source that registered as an effect
#                 would be handed an input it never asked for
#   bench         the render cost, for the record; not pass/fail
#
# The last five are release-job work done locally on purpose. A check that
# only runs in CI, after a tag, is a check that will catch you after the tag.
#
# ------------------------------------------------- why the build is deleted
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is where
# the architecture list lives. A developer who configured once with
# `-DCMAKE_OSX_ARCHITECTURES=arm64` for a fast iteration loop leaves a tree
# where this script happily rebuilds, finds a single-architecture binary and
# reports it as a defect in the source. So: fresh configure, every time.
#
# This is bash, not zsh, on purpose: the pipe step reads PIPESTATUS.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

FILES = [
	"source/Shaders.cpp",
]

# A shader IS several adjacent raw strings here -- MSVC caps one literal at
# about 16 KB (C2026) -- so everything up to the terminating semicolon is
# joined. Lose this join and the check reports a syntax error in the middle
# of a function.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -lt 2 ]; then
		# Fewer than two shaders is a FAILURE, not a pass: there is a vertex
		# shader and a fragment shader, and the fragment one is the join above.
		printf '   only %d shader(s) extracted -- the extraction has gone stale\n' "$n"
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# The browser demo's copy of the shaders.
#
# demo/plugin.js carries source/Shaders.cpp's GLSL -- the vertex shader and the
# fragment shader's two raw strings -- and two copies of a shader drift quietly,
# because a demo that renders a plausible pattern editor looks exactly like one
# that renders the right one. pntest has no idea the page exists, so this is the
# only thing holding the page's claim to be running the plugin's own shader.
#---------------------------------------------------------------------------
step "demo shaders"
if [ -f demo/tools/check_shaders.py ]; then
	if out="$( python3 demo/tools/check_shaders.py 2>&1 )"; then
		pass "$( printf '%s' "$out" | tail -1 )"
	else
		fail "the demo's shader copies have drifted from source/Shaders.cpp"
		printf '%s\n' "$out" | tail -12
	fi
else
	printf '   skipped: demo/tools/check_shaders.py is not present\n'
fi

step "build (fresh, universal)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/tmp/pattern-configure.log 2>&1 \
   && cmake --build "$BUILD" --parallel >/tmp/pattern-build.log 2>&1; then
	pass "configured and built"
else
	fail "build failed -- see /tmp/pattern-build.log"
	tail -25 /tmp/pattern-build.log
	exit 1
fi

PNTEST="$BUILD/pntest"

#---------------------------------------------------------------------------
# The tracker. None of these opens a GL context.
#---------------------------------------------------------------------------
step "tracker (no GL)"
for t in timing onset prime pitch ring detected groove names font; do
	log="/tmp/pattern-$t.log"
	if "$PNTEST" "--$t" >"$log" 2>&1; then
		pass "pntest --$t"
	else
		fail "pntest --$t -- see $log"
		grep -E "FAIL" "$log" | head -6
	fi
done

#---------------------------------------------------------------------------
# The release video's soundtrack, when the backend checkout is on this
# machine: a real file, 48 s of synthesised 125 BPM groove, on which v0.1.0
# read 62 and 100. Optional because CI has no backend checkout.
#---------------------------------------------------------------------------
step "the release video's soundtrack (no GL)"
SOUNDTRACK="${SOUNDTRACK:-$HOME/Projects/infrastructure/stoatworks-backend/video/projects/pattern/footage/soundtrack.wav}"
if [ -f "$SOUNDTRACK" ]; then
	for fps in 30 60; do
		if "$PNTEST" --tempo-wav "$SOUNDTRACK" --truth 125 --from 4 --fps $fps >/tmp/pattern-soundtrack-$fps.log 2>&1; then
			pass "the video's soundtrack at $fps fps: $( grep -E 'readings' /tmp/pattern-soundtrack-$fps.log | sed 's/^ *ok *//' )"
		else
			fail "the video's soundtrack at $fps fps -- see /tmp/pattern-soundtrack-$fps.log"
		fi
	done
else
	printf '   skipped: the video soundtrack is not at %s\n' "$SOUNDTRACK"
fi

step "grid (the one check that reads a rasteriser)"
if "$PNTEST" --grid >/tmp/pattern-grid.log 2>&1; then
	pass "every glyph on the whole-pixel grid at every Scale, at 640x360 and 320x180"
else
	fail "pntest --grid -- see /tmp/pattern-grid.log"
	grep -E "FAIL" /tmp/pattern-grid.log | head -8
fi
# And on Apple's software renderer, which is what a GPU-less CI runner gets.
if PNTEST_RENDERER=software "$PNTEST" --grid >/tmp/pattern-grid-sw.log 2>&1; then
	pass "the same on $( sed -n 's/^renderer: //p' /tmp/pattern-grid-sw.log | head -1 )"
else
	fail "PNTEST_RENDERER=software pntest --grid -- see /tmp/pattern-grid-sw.log"
	grep -E "FAIL" /tmp/pattern-grid-sw.log | head -8
fi

#---------------------------------------------------------------------------
# The pipe. Whole frames, and the right exit status when the reader goes.
#
# `head -c 1` reads one byte and closes the pipe. Twenty 320x180 frames are
# 4.6 MB, far more than a pipe buffer holds, so the harness cannot finish
# before the reader has gone: its next write fails with EPIPE (SIGPIPE is
# ignored) and it must exit 1. PIPESTATUS is bash's; zsh has no such thing,
# which is why this file says bash on line one.
#---------------------------------------------------------------------------
step "pipe"
bytes=$("$PNTEST" --pipe --size 64x36 --frames 3 --audio beats 2>/dev/null | wc -c | tr -d ' ')
if [ "$bytes" = "27648" ]; then
	pass "--pipe writes three whole 64x36 RGBA frames"
else
	fail "--pipe wrote $bytes bytes, expected 27648"
fi
"$PNTEST" --pipe --size 320x180 --frames 20 --audio beats 2>/tmp/pattern-pipe.log | head -c 1 >/dev/null
pipe_status=${PIPESTATUS[0]}
if [ "$pipe_status" = "1" ]; then
	pass "a reader that hangs up gets exit 1 ($(tr -d '\n' </tmp/pattern-pipe.log))"
else
	fail "a reader that hung up got exit $pipe_status, expected 1"
fi

step "sweep"
for size in 640x360 320x180; do
	if python3 tools/sweep.py --binary "$PNTEST" --size $size --jobs 4 >/tmp/pattern-sweep-$size.log 2>/dev/null; then
		pass "$size: $( tail -1 /tmp/pattern-sweep-$size.log )"
	else
		fail "tools/sweep.py reports a dead control at $size -- see /tmp/pattern-sweep-$size.log"
		tail -4 /tmp/pattern-sweep-$size.log | sed 's/^/   /'
	fi
done

BUNDLE="$BUILD/Pattern.bundle"
BIN="$BUNDLE/Contents/MacOS/Pattern"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Pattern.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		out=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$out" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*PASS*) pass "registers, instantiates and lights pixels" ;;
			*"id:"*) fail "registers but oxbow reports FAIL -- see: $OXBOW selftest $BUNDLE" ;;
			*) fail "oxbow did not recognise the bundle" ;;
		esac
		# And the identity a host reads, which nothing else here checks.
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		case "$probe" in *"id:          PN01"*) pass "id is PN01" ;; *) fail "wrong FFGL id" ;; esac
		case "$probe" in *"name:        SW Pattern"*) pass "name is SW Pattern" ;; *) fail "wrong plugin name" ;; esac
		case "$probe" in *"type:        source"*) pass "type is source" ;; *) fail "wrong plugin type" ;; esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

step "bench"
"$PNTEST" --bench 2>/dev/null | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
