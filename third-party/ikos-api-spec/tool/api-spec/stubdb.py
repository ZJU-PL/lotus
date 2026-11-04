#!/usr/bin/env python3
###############################################################################
#
# IKOS API spec -- stub database (cross-program body reuse)
#
# Contact: ikos@lists.nasa.gov
#
# Notices:
#
# Copyright (c) 2018-2023 United States Government as represented by the
# Administrator of the National Aeronautics and Space Administration.
# All Rights Reserved.
#
# Disclaimers:
#
# No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF
# ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
# LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
# SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
# PARTICULAR PURPOSE, OR NONINFRINGEMENT. THE SUBJECT SOFTWARE WOULD CONFORM
# TO THE DOCUMENTATION. NO GOVERNMENT AGENCY, NOR ANY EMPLOYEE THEREOF,
# ASSUMES ANY LEGAL LIABILITY OR RESPONSIBILITY FOR THE ACCURACY,
# COMPLETENESS, OR USEFULNESS OF ANY INFORMATION, APPARATUS, PRODUCT, OR
# PROCESS DISCLOSED. WOULD NOT INFRINGE PRIVATELY OWNED RIGHTS.
#
###############################################################################
"""Stub database: gate-passing stub bodies shared across programs.

Generation is expensive -- an LLM per function, a differential battery per
candidate -- and its evidence is program-independent: a body that passed
the battery for one program's ``declare i64 @strtol(...)`` line is exactly
as valid for another program with the same declaration.  This module makes
those bodies reusable:

* identity: source name + signature key (the same ruler ``ikos-stub link``
  applies to the linked artifact), so a variant declaration never silently
  picks up a foreign body;
* domain axis: a stub generated for a specific numeric backend
  (``--domain congruence``, the token of the analysis run's ``-d`` flag)
  is only sound under that backend's join semantics, so it is stored under
  a domain-tagged key and is never handed to a run with another domain;
  portable entries (the default ``--domain all``, judged under both join
  families) keep the bare signature key -- the legacy shape -- and match
  every query;
* rank: a body verified by the differential battery (rank 2) beats a
  compile-only body from ``--no-difftest`` runs (rank 1); recording at
  equal or lower rank keeps the existing entry;
* layout: ONE SMALL FILE PER FUNCTION, ``entries/<source-name>.json``,
  each holding that function's signature-key -> entry table.  The store is
  built for tens of thousands of entries: a record rewrites one ~KB shard
  (independent of the database size -- a 500-function run never rescans or
  rewrites the rest), a query reads one shard, and process start loads
  nothing eagerly.  Per-function files are also the git-friendly shape:
  diffs are additive, merges are per function, never whole-file conflicts.

A database hit is cheap but not free of verification: the pipeline
re-splices the body into the CURRENT program's rendered skeleton and
re-runs the lexical guard, the compile gate and the signature gate before
installing it (see ikos-api-spec._db_reuse); only the differential battery
is not re-run, because its evidence does not depend on the calling
program.

Durability: every shard is flushed with os.replace (atomic); single-writer
stance, the same concurrency model as the response cache.  A legacy
single-file ``index.json`` (the pre-sharding layout) is still read
transparently and shadowed by shards as they are written; delete it once
everything has been re-recorded.

The store is deliberately isolated behind this module's interface so a
server-backed implementation can replace it without touching the pipeline.
"""
import json
import os
import os.path
import re
import sys
import time
import zlib

DEFAULT_DB_DIR = '~/.cache/ikos-api-spec/stubdb'

ENTRIES_DIR = 'entries'
LEGACY_INDEX = 'index.json'

RANK_VERIFIED = 2
RANK_COMPILED = 1

_UNSAFE_CHAR_RE = re.compile(r'[^A-Za-z0-9_.$-]')


PORTABLE_DOMAIN = 'all'


def slot_key(sig_key, domain):
    """The shard-table key of one declaration slot under a domain choice.

    Portable bodies (domain 'all', the default) keep the bare signature
    key -- exactly the legacy shape, so old shards and old single-file
    databases read as portable.  A domain-targeted body lives under a
    tagged key beside it: same declaration, different join semantics.
    """
    if domain == PORTABLE_DOMAIN:
        return sig_key
    return '%s|domain=%s' % (sig_key, domain)


def shard_name(source_name):
    """File name of one function's shard.

    Clean names map to themselves; anything else is sanitized and tagged
    with a crc32 of the original name so two different odd names never
    fold onto the same shard.
    """
    safe = _UNSAFE_CHAR_RE.sub('_', source_name)
    if not safe or safe in ('.', '..') or safe != source_name:
        safe = '%s-%08x' % (safe or 'fn',
                            zlib.crc32(source_name.encode('utf-8')) & 0xffffffff)
    return safe + '.json'


def _rank_of(entry):
    return RANK_VERIFIED if entry.get('verified') else RANK_COMPILED


def _default_warn(message):
    sys.stderr.write('warning: %s\n' % message)


class StubDB:
    """A directory of per-function shards: entries/<source name>.json.

    Each shard maps a slot key -> {'ir_name', 'declare', 'body', 'model',
    'verified', 'domain', 'time'}, one slot per (declaration, domain
    choice): portable bodies under the bare signature key, --domain
    targeted bodies under slot_key(), each holding the highest-rank body
    recorded so far.  Shards are cached in memory after the first read;
    nothing is loaded eagerly.
    """

    def __init__(self, directory, warn=None):
        self.dir = os.path.expanduser(directory)
        self.path = self.dir  # the directory identifies the database
        self.entries_dir = os.path.join(self.dir, ENTRIES_DIR)
        self.warn = warn or _default_warn
        self._shards = {}   # source name -> table, filled on demand
        self._legacy_data = None  # pre-sharding index.json, loaded on demand

    # ------------------------------------------------------------- reading

    def query(self, source_name, sig_key, domain=PORTABLE_DOMAIN):
        """The entry recorded for (source name, signature key, domain).

        A domain-targeted query takes its own entry first, then the
        portable one (sound under every backend).  A portable query takes
        ONLY the portable entry: a body admitted solely by the congruence
        class, say, must not leak into a run that will be judged under
        both families.
        """
        shard = self._shard(source_name)
        if domain != PORTABLE_DOMAIN:
            entry = shard.get(slot_key(sig_key, domain))
            if entry is not None:
                return entry
        entry = shard.get(sig_key)
        if entry is not None:
            return entry
        table = self._legacy().get(source_name)
        if isinstance(table, dict):
            entry = table.get(sig_key)
            if isinstance(entry, dict) and 'body' in entry:
                return entry
        return None

    # ------------------------------------------------------------- writing

    def record(self, source_name, ir_name, declare, body, sig_key, model,
               verified, domain=PORTABLE_DOMAIN):
        """Remember a gate-passing body; a higher rank replaces, an equal
        or lower rank keeps the existing entry."""
        table = self._shard(source_name)
        key = slot_key(sig_key, domain)
        existing = table.get(key)
        rank = RANK_VERIFIED if verified else RANK_COMPILED
        if existing is not None and _rank_of(existing) >= rank:
            return
        table[key] = {'ir_name': ir_name,
                      'declare': declare,
                      'body': body,
                      'model': model,
                      'verified': bool(verified),
                      'domain': domain,
                      'time': int(time.time())}
        self._flush(source_name)

    # ------------------------------------------------------------- plumbing

    def _shard(self, source_name):
        if source_name not in self._shards:
            path = os.path.join(self.entries_dir, shard_name(source_name))
            self._shards[source_name] = _load_table(path)
        return self._shards[source_name]

    def _legacy(self):
        if self._legacy_data is None:
            path = os.path.join(self.dir, LEGACY_INDEX)
            try:
                with open(path) as stream:
                    data = json.load(stream)
            except (OSError, ValueError):
                data = {}
            self._legacy_data = data if isinstance(data, dict) else {}
        return self._legacy_data

    def _flush(self, source_name):
        try:
            os.makedirs(self.entries_dir, exist_ok=True)
            path = os.path.join(self.entries_dir, shard_name(source_name))
            tmp = path + '.tmp'
            with open(tmp, 'w') as stream:
                json.dump(self._shards[source_name], stream, indent=1,
                          sort_keys=True)
            os.replace(tmp, path)
        except OSError as error:
            self.warn('stub database not updated (%s: %s)' % (path, error))


def _load_table(path):
    """One shard file -> {sig key -> entry}; unreadable is just empty."""
    try:
        with open(path) as stream:
            data = json.load(stream)
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict):
        return {}
    return {key: entry for key, entry in data.items()
            if isinstance(entry, dict) and 'body' in entry}
