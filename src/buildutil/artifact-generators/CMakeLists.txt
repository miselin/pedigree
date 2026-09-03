cmake_minimum_required(VERSION 3.21)
project(PedigreeArtifactGenerators C)

add_subdirectory(../configdb configdb)
add_subdirectory(../bootstrap bootstrap)

add_custom_target(pedigree-artifact-generators ALL DEPENDS
    pedigree-configdb
    pedigree-initrd-builder)
