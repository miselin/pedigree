#!/bin/bash

# Note: this is intended to be sourced from an easy_build script, which already
# has $script_dir defined. It installs needed dependencies and then sets
# $real_os to the OS we are running on.

set -e

echo "Pedigree Easy Build script"
echo "This script will automatically install dependencies and compile Pedigree"
echo "for you."
echo

real_os=""
nosudo=0
if [ ! -e "$script_dir/.easy_os" ]; then

    confirm=""
    if [ $# == 0 ]; then
        case "$(uname -s)" in
            Darwin)
                os="osx"
                ;;
            Linux)
                os=""
                if [ -r /etc/os-release ]; then
                    . /etc/os-release
                    case "${ID:-} ${ID_LIKE:-}" in
                        *ubuntu*)
                            os="ubuntu"
                            ;;
                        *debian*)
                            os="debian"
                            ;;
                        *suse*)
                            os="opensuse"
                            ;;
                        *fedora*|*rhel*)
                            os="fedora"
                            ;;
                        *arch*)
                            os="arch"
                            ;;
                    esac
                    [ -n "$os" ] || os="${ID:-linux}"
                else
                    os="linux"
                fi
                ;;
            OpenBSD)
                os="openbsd"
                ;;
            CYGWIN*|MINGW*|MSYS*)
                os="cygwin"
                ;;
            *)
                os="$(uname -s)"
                ;;
        esac
        echo "Detected host operating system: $os"
    else
        os=$1
        if [ "$os" = "nosudo" ]; then
            os=$2
            nosudo=1
        elif [ "$os" = "noconfirm" ]; then
            os=$2
            confirm="-y"
        fi
    fi

    shopt -s nocasematch

    real_os=$os

    case $real_os in
        debian)
            # TODO: Not sure if the package list is any different for debian vs ubuntu?
            echo "Installing packages with apt-get, please wait..."
            [ $nosudo = 0 ] && sudo apt-get install $confirm libmpfr-dev \
                libmpc-dev libgmp-dev sqlite3 texinfo genisoimage u-boot-tools \
                nasm python3-requests autoconf automake cmake bison flex lcov
            ;;
        ubuntu)
            echo "Installing packages with apt-get, please wait..."
            [ $nosudo = 0 ] && sudo apt-get install $confirm libmpfr-dev \
                libmpc-dev libgmp-dev sqlite3 texinfo genisoimage e2fsprogs \
                u-boot-tools nasm python3-requests autoconf automake cmake \
                bison flex lcov
            ;;
        opensuse)
            echo "Installing packages with zypper, please wait..."
            set +e
            sudo zypper install mpfr-devel mpc-devel gmp3-devel sqlite3 \
                texinfo cmake bison flex autoconf automake nasm genisoimage
            set -e
            ;;
        fedora|redhat|centos|rhel)
            echo "Installing packages with YUM, please wait..."
            sudo yum install $confirm mpfr-devel gmp-devel libmpc-devel \
                sqlite texinfo cmake bison flex autoconf automake nasm genisoimage
            ;;
        osx|mac)
            if type port >/dev/null 2>&1; then
                echo "Installing packages with macports, please wait..."

                sudo port install mpfr libmpc gmp libiconv sqlite3 texinfo \
                    cmake bison flex cdrtools wget mtools gnutar nasm
            elif type brew >/dev/null 2>&1; then
                echo "Installing packages with Homebrew, please wait..."

                brew list cmake &>/dev/null || brew install cmake
                brew list bison &>/dev/null || brew install bison
                brew list flex &>/dev/null || brew install flex
                brew list gnu-tar &>/dev/null || brew install gnu-tar
                brew list wget &>/dev/null || brew install wget
                brew list xorriso &>/dev/null || brew install xorriso
                brew list sqlite3 &>/dev/null || brew install sqlite3
                brew list mtools &>/dev/null || brew install mtools
                brew list nasm &>/dev/null || brew install nasm
                brew list gmp &>/dev/null || brew install gmp
                brew list mpfr &>/dev/null || brew install mpfr
                brew list libmpc &>/dev/null || brew install libmpc
                brew list qemu &>/dev/null || brew install qemu
                brew list e2fsprogs &>/dev/null || brew install e2fsprogs
                brew list autoconf &>/dev/null || brew install autoconf
                brew list automake &>/dev/null || brew install automake
                brew list gettext &>/dev/null || brew install gettext

                set +e  # Avoid brew terminating the build due to link failures
                brew link -f gettext  # not linked by default
                brew link -f e2fsprogs
                set -e
            fi
            real_os="osx"
            ;;
        openbsd)
            echo "Installing packages with pkg_add, please wait..."
            sudo pkg_add cmake bison flex mtools sqlite cdrtools gmp mpfr \
                libmpc wget nasm
            ;;
        cygwin|windows|mingw)
            echo "Please ensure you use Cygwin's 'setup.exe', or some other method, to install the following:"
            echo " - Python"
            echo " - GCC & binutils"
            echo " - libgmp, libmpc, libmpfr"
            echo " - mkisofs/genisoimage"
            echo " - sqlite"
            echo " - patch"
            echo " - GNU make"
            echo "You will need to find alternative sources for the following:"
            echo " - mtools"
            echo " - CMake"
            echo " - Bison and Flex"

            real_os="cygwin"
            ;;
        arch)
            echo "Installing packages with pacman, please wait..."
            sudo pacman -S gcc binutils gmp libmpc mpfr sqlite texinfo cmake \
                bison flex autoconf automake nasm wget cdrtools mtools tar
            ;;
        *)
            echo "Operating system '$os' is not supported yet."
            echo "You will need to find alternative sources for the following:"
            echo " - Python"
            echo " - GCC & binutils"
            echo " - libgmp, libmpc, libmpfr"
            echo " - mkisofs/genisoimage"
            echo " - sqlite"
            echo " - mtools"
            echo " - CMake"
            echo " - Bison and Flex"
            echo " - wget"
            echo " - sed"
            echo
            echo "If you can modify this script to support '$os', please provide patches."
            ;;
    esac

    shopt -u nocasematch

    echo "$real_os" > "$script_dir/.easy_os"

    echo

else
    real_os=$(cat "$script_dir/.easy_os")
fi
