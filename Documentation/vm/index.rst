=====================================
Linux Memory Management Documentation
=====================================

This is a collection of documents about Linux memory management (mm) subsystem.

User guides for MM features
===========================

The following documents provide guides for controlling and tuning
various features of the Linux memory management

.. toctree::
   :maxdepth: 1

   ksm
   transhuge
   swap_numa
   zswap

Kernel developers MM documentation
==================================

The below documents describe MM internals with different level of
details ranging from notes and mailing list responses to elaborate
descriptions of data structures and algorithms.

.. toctree::
   :maxdepth: 1

   active_mm
   balance
   cleancache
   frontswap
   highmem
   hmm
   hwpoison
   hugetlbfs_reserv
   mmu_notifier
   numa
   overcommit-accounting
   page_migration
   page_frags
   page_owner
   remap_file_pages
   slub
   split_page_table_lock
   unevictable-lru
   z3fold
   zsmalloc
