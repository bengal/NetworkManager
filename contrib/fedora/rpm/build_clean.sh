#!/bin/bash


die() {
    echo "$*" >&2
    exit 1
}

usage() {
    echo "USAGE: $0 [-h|--help|-?|help] [-f|--force] [-c|--clean] [-Q|--quick] [-S|--srpm] [-N|--no-dist] [[-w|--with OPTION] ...] [[-W|--without OPTION] ...]"
    echo
    echo "Does all the steps from a clean git working directory to an RPM of NetworkManager"
    echo
    echo "Options:"
    echo "  --force: force build, even if working directory is not clean and has local modifications"
    echo "  --clean: run \`git-clean -fdx :/\` before build"
    echo "  --quick: only run \`make dist\` instead of \`make distcheck\`"
    echo "  --srpm: only build the SRPM"
    echo "  --no-dist: skip creating the source tarball if you already did \`make dist\`"
    echo "  --with \$OPTION: pass --with \$OPTION to rpmbuild. For example --with debug"
    echo "  --without \$OPTION: pass --without \$OPTION to rpmbuild. For example --without debug"
}


ORIGDIR="$(readlink -f "$PWD")"
SCRIPTDIR="$(dirname "$(readlink -f "$0")")"
GITDIR="$(cd "$SCRIPTDIR" && git rev-parse --show-toplevel || die "Could not get GITDIR")"


[[ -x "$SCRIPTDIR"/build.sh ]] || die "could not find \"$SCRIPTDIR/build.sh\""

cd "$GITDIR" || die "could not change to $GITDIR"

IGNORE_DIRTY=0
GIT_CLEAN=0
QUICK=0
NO_DIST=0
WITH_LIST=()

_next_with=
for A; do
    if [ -n "$_next_with" ]; then
        WITH_LIST=("${WITH_LIST[@]}" "$_next_with" "$A")
        _next_with=
        continue
    fi
    case "$A" in
        -h|--help|-\?|help)
            usage
            exit 0
            ;;
        -f|--force)
            IGNORE_DIRTY=1
            ;;
        -c|--clean)
            GIT_CLEAN=1
            ;;
        -Q|--quick)
            QUICK=1
            ;;
        -S|--srpm)
            BUILDTYPE=SRPM
            ;;
        -N|--no-dist)
            NO_DIST=1
            IGNORE_DIRTY=1
            ;;
        -w|--with)
            _next_with=--with
            ;;
        -W|--without)
            _next_with=--without
            ;;
        *)
            usage
            die "Unexpected argument \"$A\""
            ;;
    esac
done

test -n "$_next_with" && die "Missing argument to $_next_with"

if [[ $GIT_CLEAN == 1 ]]; then
    git clean -fdx :/
fi

if [[ $IGNORE_DIRTY != 1 ]]; then
    # check for a clean working directory.
    # We ignore the /contrib directory, because this is where the automation
    # scripts and the build results will be.
    if [[ "x$(LANG=C git clean -ndx | grep '^Would \(remove contrib/\|skip repository libgsystem/\).*$' -v)" != x ]]; then
        die "The working directory is not clean. Refuse to run. Try \`$0 --force\`, \`$0 --clean\`, or \`git clean -e :/contrib -dx -n\`"
    fi
    if [[ "x$(git status --porcelain)" != x ]]; then
        die "The working directory has local changes. Refuse to run. Try \`$0 --force\`"
    fi
fi

export CFLAGS="-ggdb -fno-omit-frame-pointer -DVALGRIND=1 -fsanitize=address"
export CPPFLAGS="-DVALGRIND=1"
export LDFLAGS="-fsanitize=address"
export ASAN_OPTIONS="detect_leaks=0"

if [[ $NO_DIST != 1 ]]; then
    ./autogen.sh --enable-gtk-doc || die "Error autogen.sh"

    if [[ $QUICK == 1 ]]; then
        make -C shared || die "Error make -C shared"
        make -C introspection || die "Error make -C introspection"
        make -C libnm-core || die "Error make -C libnm-core"
        make -C libnm || die "Error make -C libnm"
        make -C libnm-util || die "Error make -C libnm-util"
        make -C libnm-glib || die "Error make -C libnm-glib"
        make dist || die "Error make distcheck"
    else
        make -j 10 || die "Error make"
        make dist || die "Error make dist"
    fi
fi

export BUILDTYPE
export NM_RPMBUILD_ARGS="${WITH_LIST[@]}"

"$SCRIPTDIR"/build.sh

