/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"

#include "FatFilesystem.h"
#include "FatSymlink.h"

void FatFilesystem::registerNode(File* file) {
  if ((!file->isDirectory() && !file->isSymlink()) || file->getName() == "." ||
      file->getName() == "..")
    return;

  LockGuard<Mutex> registry(m_StateLock);
  if (m_NodeAliases.lookup(file))
    return;
  const uintptr_t inode = file->getInode();
  NodeState* state = m_NodeStates.lookup(inode);
  if (!state) {
    state = new NodeState;
    state->inode = inode;
    if (file->isDirectory()) {
      auto* directory = static_cast<FatDirectory*>(file);
      state->directoryCluster = directory->getDirCluster();
      state->directoryOffset = directory->getDirOffset();
      state->unlinked = directory->m_Unlinked;
    } else {
      auto* symlink = static_cast<FatSymlink*>(file);
      state->directoryCluster = symlink->getDirCluster();
      state->directoryOffset = symlink->getDirOffset();
      state->unlinked = symlink->m_Unlinked;
    }
    m_NodeStates.insert(inode, state);
  }
  state->aliases.pushBack(file);
  m_NodeAliases.insert(file, state);

  // A resolver can have read the old slot before a rename reached this registry.
  if (file->isDirectory()) {
    auto* directory = static_cast<FatDirectory*>(file);
    directory->setDirCluster(state->directoryCluster);
    directory->setDirOffset(state->directoryOffset);
    directory->m_Unlinked = state->unlinked;
    if (state->unlinked)
      directory->markDetached();
  } else {
    auto* symlink = static_cast<FatSymlink*>(file);
    symlink->setDirCluster(state->directoryCluster);
    symlink->setDirOffset(state->directoryOffset);
    symlink->m_Unlinked = state->unlinked;
  }
}

void FatFilesystem::releaseNode(File* file) {
  LockGuard<Mutex> mutation(m_FileMutationLock);
  NodeState* retired = nullptr;
  {
    LockGuard<Mutex> registry(m_StateLock);
    NodeState* state = m_NodeAliases.lookup(file);
    if (!state)
      return;
    m_NodeAliases.remove(file);
    for (size_t i = 0; i < state->aliases.count(); ++i) {
      if (state->aliases[i] == file) {
        state->aliases.erase(i);
        break;
      }
    }
    if (state->aliases.count())
      return;
    if (m_NodeStates.lookup(state->inode) == state)
      m_NodeStates.remove(state->inode);
    retired = state;
  }

  // Reclamation uses only the saved allocation identity after alias removal.
  if (retired->unlinked && !m_bReadOnly && !releaseClusterChain(retired->inode, false))
    ERROR("FAT: orphan node allocation reclamation needs a FAT retry");
  delete retired;
}

void FatFilesystem::moveNonFileNode(File* file, uint32_t cluster, uint32_t offset) {
  LockGuard<Mutex> registry(m_StateLock);
  const auto moveAlias = [cluster, offset](File* alias) {
    if (alias->isDirectory()) {
      auto* directory = static_cast<FatDirectory*>(alias);
      directory->setDirCluster(cluster);
      directory->setDirOffset(offset);
    } else {
      auto* symlink = static_cast<FatSymlink*>(alias);
      symlink->setDirCluster(cluster);
      symlink->setDirOffset(offset);
    }
  };
  NodeState* state = m_NodeAliases.lookup(file);
  if (!state) {
    moveAlias(file);
    return;
  }
  state->directoryCluster = cluster;
  state->directoryOffset = offset;
  for (File* alias : state->aliases)
    moveAlias(alias);
}

void FatFilesystem::unlinkNonFileNode(File* file) {
  LockGuard<Mutex> registry(m_StateLock);
  const auto unlinkAlias = [](File* alias) {
    if (alias->isDirectory()) {
      auto* directory = static_cast<FatDirectory*>(alias);
      directory->m_Unlinked = true;
      directory->markDetached();
    } else {
      static_cast<FatSymlink*>(alias)->m_Unlinked = true;
    }
  };
  NodeState* state = m_NodeAliases.lookup(file);
  if (!state) {
    unlinkAlias(file);
    return;
  }
  // Its allocation remains reserved, so stale resolvers must join this orphan.
  state->unlinked = true;
  for (File* alias : state->aliases)
    unlinkAlias(alias);
}
