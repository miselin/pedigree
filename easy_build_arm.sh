#!/bin/bash

# Script that can be run to set up a Pedigree repository for building with minimal
# effort.

old=$(pwd)
script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P) && script_dir=$script_dir
cd $old

COMPILER_DIR=$script_dir/pedigree-compiler
. $script_dir/build-etc/travis.sh

set -e

. $script_dir/scripts/easy_build_deps.sh

echo "Please wait, checking for a working cross-compiler."
echo "If none is found, the source code for one will be downloaded, and it will be"
echo "compiled for you."

# Special parameters for some operating systems when building cross-compilers
case $real_os in
    osx)
        compiler_build_options="$compiler_build_options osx-compat"
        ;;
esac

# Install cross-compilers
"$script_dir/scripts/checkBuildSystemNoInteractive.pl" arm-pedigree $COMPILER_DIR $compiler_build_options

old=$(pwd)

# Fix up POSIX headers which sometimes get a recursive symlink.
rm -f src/subsys/posix/include/include || true

set +e

# Update the local working copy only if it is clean.
changed=`git status -s -uno`
if [ -z "$changed" ]; then
    git pull --rebase > /dev/null 2>&1
fi

if [ -d "src/modules/drivers/cdi" ]; then
    cd src/modules/drivers/cdi
    git pull || echo "Failed to update cdi."
    cd ${old}
else
    git clone git://git.tyndur.org/cdi.git src/modules/drivers/cdi || echo "Failed to clone cdi, cdi will not be part of your build."
fi

set -e

echo
echo "Configuring the Pedigree UPdater..."

$script_dir/setup_pup.py arm
$script_dir/run_pup.py sync

# Needed for libc
$script_dir/run_pup.py install ncurses

# Build Pedigree.
mkdir -p build-host && cd build-host
cmake $TRAVIS_OPTIONS ..
make
cd ..

mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../build-etc/cmake/pedigree_arm.cmake -DIMPORT_EXECUTABLES=../build-host/HostUtilities.cmake $TRAVIS_OPTIONS ..

# Build libc/libm
make libc
cd ..

# Pull down libtool.
$script_dir/run_pup.py install libtool

# Enforce using our libtool.
export LIBTOOL=$script_dir/../images/local/applications:$PATH

# Build GCC again with access to the newly built libc.
# This will create a libstdc++ that can be used by pedigree-apps to build GCC
# again, this time with a shared libstdc++. pedigree-apps should then build GCC
# again to build it against the shared libstdc++. Once a working shared
# libstdc++ exists, the static one built here is no longer relevant.
# What a mess!
$script_dir/scripts/checkBuildSystemNoInteractive.pl arm-pedigree $COMPILER_DIR $compiler_build_options "libcpp"

set +e

echo
echo "Ensuring CDI is up-to-date."

# Setup all submodules, make sure they are up-to-date
git submodule init > /dev/null 2>&1
git submodule update > /dev/null 2>&1

echo
echo "Installing a base set of packages..."

$script_dir/run_pup.py install pedigree-base
$script_dir/run_pup.py install libpng
$script_dir/run_pup.py install libfreetype
$script_dir/run_pup.py install libiconv
$script_dir/run_pup.py install zlib

$script_dir/run_pup.py install bash
$script_dir/run_pup.py install coreutils
$script_dir/run_pup.py install fontconfig
$script_dir/run_pup.py install pixman
$script_dir/run_pup.py install cairo
$script_dir/run_pup.py install expat
$script_dir/run_pup.py install mesa
$script_dir/run_pup.py install gettext

$script_dir/run_pup.py install pango
$script_dir/run_pup.py install glib
$script_dir/run_pup.py install libpcre
$script_dir/run_pup.py install harfbuzz
$script_dir/run_pup.py install libffi
$script_dir/run_pup.py install dialog

# Install GCC to pull in shared libstdc++.
$script_dir/run_pup.py install gcc

set -e

echo
echo "Beginning the Pedigree build."
echo

# Build full kernel
cd build
make

cd "$old"

echo
echo
echo "Pedigree is now ready to be built without running this script."
echo "To build in future, run the following command in the '$script_dir' directory:"
echo "scons"
echo
echo "If you wish, you can continue to run this script. It won't ask questions"
echo "anymore, unless you remove the '.easy_os' file in '$script_dir'."
echo
echo "You can also run scons --help for more information about options."
echo
echo "Patches should be posted in the issue tracker at http://pedigree-project.org/projects/pedigree/issues"
echo "Support can be found in #pedigree on irc.freenode.net."
echo
echo "Have fun with Pedigree! :)"
