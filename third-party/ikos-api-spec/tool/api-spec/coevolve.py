#!/usr/bin/env python3
###############################################################################
#
# IKOS API spec -- dual-agent co-evolutionary game (potential-game engine)
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
# LIMITED TO, ANY WARRANTY OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
# PURPOSE, OR NONINFRINGEMENT. THE SUBJECT SOFTWARE WOULD CONFORM TO
# SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
# PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY, THAT IF
# PROVIDED THE DOCUMENTATION WILL CONFORM TO THE DOCUMENTATION.  NO
# GOVERNMENT AGENCY, NOR ANY EMPLOYEE THEREOF, ASSUMES ANY LEGAL LIABILITY
# OR RESPONSIBILITY FOR THE ACCURACY, COMPLETENESS, OR USEFULNESS OF ANY
# INFORMATION, APPARATUS, PRODUCT, OR PROCESS DISCLOSED, OR REPRESENTS THAT
# ITS USE WOULD NOT INFRINGE PRIVATELY OWNED RIGHTS.
#
###############################################################################
"""Dual-agent co-evolutionary game around the difftest battery.

The generate pipeline already contains two generative agents:

* player A -- the execution agent: the model behind the difftest driver and
  its inputs.  It generates the program's execution inputs and collects the
  execution data;
* player B -- the invariant agent: the model behind the stub body.  Its
  invariant IS the ``__ikos_*`` stub body.

This module makes their interaction an explicit repeated game with a
measurable potential, so the pipeline output moves from *reliable* (an
over-approximation, which the difftest gate already guarantees) toward
*precise* (an over-approximation close to the observed behavior):

    state       s_t = (T_t, I_t, I^A_t, I^B_t)
                  T  = accumulated input pool, I = current stub body,
                  I^A = execution-data abstraction, I^B = spec abstraction
    potential   Phi(s) = d(I^A, I^B) + lambda * Complexity(I)
                                  - mu * Coverage(T)
    payoffs     u_A = Delta-Phi + alpha * ExplorationBonus(a_t)
                u_B = Delta-Phi - beta  * ComplexityPenalty(b_t)
                (defaults alpha = beta = 0: identical interests, -Phi an
                EXACT potential -- the default configuration sits inside
                the theorem.  alpha/beta > 0 are delta-approximate
                ablation arms; exploration then leaves the payoff and
                works as A's tie-break heuristic)
    schedule    alternating moves, A first: A plays -> the battery settles
                -> B plays -> the battery settles, one round = two stages.
                Asynchronous better-response is exactly the dynamic under
                which an exact potential game's FIP guarantees convergence;
                a simultaneous update whose oscillations FIP says nothing
                about never runs.
    acceptance  a B revision only settles to stay when its priced u_B
                exceeds tau (default -epsilon: an epsilon-tolerant better
                response -- a revision that does not worsen Phi beyond
                the equilibrium tolerance stays; tau = 0 is the strict
                arm where only Phi-lowering revisions stay).  The pricing
                runs before the revision may settle, and w_v's domination
                of every non-violation term makes an inconsistency-
                creating revision price a full violation weight below
                zero -- far under any tau >= -epsilon -- so it is ALWAYS
                reverted, whatever else the state says.  The harness
                reverts a rejected move to the standing body and marks it
                '-rejected'.  Evidence never reverts: A's inputs
                and the veto battery's inputs stay in the pool whatever
                they expose -- an input that reveals an inconsistency is
                the most valuable move in the game even though it raises
                Phi.
    strategies  A: targeted-boundary / distribution-shift / shape-variant /
                  replay-mutate / deterministic-sweep
                B: tighten-interval / tighten-congruence / shrink-havoc /
                  drop-null-capability / simplify / widen-on-inconsistency
    stopping    |Delta-Phi| < epsilon over one full A+B cycle with no
                improvement, or an empirical epsilon-Nash over realized
                actions -- declared only after a deterministic boundary
                battery fails to falsify the equilibrium.
    budget      --co-rounds bounds the rounds; at exhaustion the game hands
                back the last battery-passing body (re-verified on the
                final pool) as the reliable floor, so a bounded number of
                iterations always returns a sound stub.

Every metric (d, complexity, coverage, bonuses, payoffs) is computed by this
deterministic harness; the LLM agents only choose strategies and produce
artifacts.  The soundness guarantee stays OUTSIDE the game: whatever the
game state, the body that is finally installed must pass the difftest
battery, and a round-0 body that already fails is handed straight to the
standard repair loop.

Terminology (kept aligned with the paper): inconsistency signal, interval
discrepancy, potential, strategy update, equilibrium.
"""
import json
import math
import os
import os.path
import random
import re
import shutil
import tempfile

import difftest

# marks the prompt text of the game rounds (the artifacts are not cached:
# each round's prompt depends on the game state)
CO_PROMPT_VERSION = '8-fresh-allocation'

CO_DEFAULTS = {'rounds': 3, 'attack_batch': 3,
               'a_policy': 'greedy', 'b_policy': 'greedy',
               'lambda': 0.1, 'mu': 0.2, 'alpha': 0.0, 'beta': 0.0,
               'epsilon': 0.02, 'tau': None}

# an inconsistency signal weighs w_v on top of its component's normalized
# distance, bounding every component's contribution to [0, 1 + w_v].  w_v
# sits ABOVE the dynamic range of every non-violation term of Phi (each
# component's non-violation distance <= 1, lambda * Complexity <= ~1 for
# any lexically sane body, mu * Coverage <= mu): an arithmetic
# lexicographic ordering where no accumulation of slack-reduction can pay
# for one inconsistency signal, in Phi or in any Delta-Phi.  That
# domination is what makes the tau acceptance rule's verdict on a
# violating revision (always reject) independent of the rest of the state.
# Configuration invariant for sweeps: keep w_v > lambda * K_max + mu + 1.
VIOLATION_WEIGHT = 8.0

# a claimed face is touched when the execution data comes within
# max(1, width / 1024) of its boundary
FACE_WIDTH_DIVISOR = 1024

A_STRATEGIES = ('targeted-boundary', 'distribution-shift', 'shape-variant',
                'replay-mutate', 'deterministic-sweep')
A_FIXED_STRATEGY = 'replay-mutate'

B_STRATEGIES = ('tighten-interval', 'tighten-congruence', 'shrink-havoc',
                'drop-null-capability', 'simplify', 'widen-on-inconsistency',
                'tighten-relational')
B_FIXED_STRATEGY = 'tighten-interval'

HELPER_CALL_RE = re.compile(r'__ikos_\w+\s*\(')
CONSTANT_RE = re.compile(r'0[xX][0-9a-fA-F]+|(?<![\w.])-?\d+')


def _co(opt, name):
    return getattr(opt, 'co_' + name, CO_DEFAULTS[name])


#############################
# Abstractions I^A and I^B #
#############################

def _kind_of(ret_kind):
    if ret_kind == 'pointer':
        return 'pointer'
    if isinstance(ret_kind, tuple):
        return 'int'
    return ret_kind  # 'float', 'void', 'other'


def _blank_abstraction(kind):
    return {'kind': kind, 'values': [], 'classes': set(), 'offsets': {},
            'wrote': {}, 'wrote_unknown': [], 'data': {},
            'content_values': {},
            # analyzer-real axes (the ikos battery): the claimed return
            # interval as ikos computed it, per-tag pointee intervals,
            # and the ikos-PROVED exclusion flags per axis
            'ret_ikos': [], 'cells_ikos': {}, 'cell_flags': {},
            'ret_flag': False}


def _usable(record):
    return record['verdict'] not in ('n/a', 'unverified') \
        and record['detail']['real'] is not None


def _absorb_ikos(abs_, ik):
    """Fold one input's ikos judge summary into the spec abstraction.

    `ret_ikos` becomes the claimed return hull (bounds None = +inf,
    '-inf' = -inf, mapped to the widest finite span); `cells_ikos` the
    claimed pointee intervals; the flags record ikos-PROVED exclusions
    so the components can raise them as violations.
    """
    values = ik.get('values') or {}

    def bounds(interval):
        lo, hi = interval
        lo = lo if isinstance(lo, int) else -(1 << 62)
        hi = hi if isinstance(hi, int) else (1 << 62)
        return lo, hi

    claimed = values.get('r')
    if isinstance(claimed, list) and len(claimed) == 2:
        abs_['ret_ikos'].append(bounds(claimed))
    elif isinstance(claimed, int):
        abs_['ret_ikos'].append((claimed, claimed))
    for name, interval in values.items():
        if not name.startswith('cell_') \
                or not isinstance(interval, list) or len(interval) != 2:
            continue
        stem, _, off = name[5:].rpartition('_')
        if not off.isdigit():
            continue
        lo, hi = bounds(interval)
        table = abs_['cells_ikos'].setdefault(stem, {})
        prev = table.get(int(off))
        table[int(off)] = (min(prev[0], lo), max(prev[1], hi)) \
            if prev else (lo, hi)
    probes = {p['line']: p for p in ik.get('probes', ())}
    for line, status in (ik.get('probe_status') or {}).items():
        if status != 0:
            continue
        probe = probes.get(int(line))
        if probe is None:
            continue
        if probe['kind'] == 'ret':
            abs_['ret_flag'] = True
        else:
            # same stem the cell_ variable names carry
            stem = re.sub(r'\W', '_', probe['tag']).strip('_') or 'buf'
            abs_['cell_flags'][stem] = True


def _absorb_return(abs_, obs, ret_kind, events=()):
    """Fold one run's return observation into the abstraction."""
    if abs_['kind'] == 'int':
        if obs['ret'] in (None, 'void') or len(obs['ret']) > 8:
            return
        value = difftest.trunc_int(obs['ret'], ret_kind[1])
        if value is not None:
            abs_['values'].append(value)
    elif abs_['kind'] == 'pointer' and obs['ret'] not in (None, 'void'):
        cls = difftest.classify_claimed_pointer(obs['ret'], obs['bufregs'],
                                                 events)
        if cls[0] == 'NULL':
            abs_['classes'].add('NULL')
        elif cls[0] == 'BUF':
            abs_['classes'].add(('BUF', cls[1]))
            lo, hi = abs_['offsets'].get(cls[1], (cls[2], cls[2]))
            abs_['offsets'][cls[1]] = (min(lo, cls[2]), max(hi, cls[2]))
        elif cls[0] == 'FRESH':
            abs_['classes'].add('FRESH')
        else:
            abs_['classes'].add('OTHER')  # fresh block: vocabulary gap


def real_abstraction(records, ret_kind):
    """I^A: the execution-data abstraction over all usable records."""
    abs_ = _blank_abstraction(_kind_of(ret_kind))
    for record in records:
        if not _usable(record):
            continue
        obs = record['detail']['real']
        abs_['wrote_unknown'] += obs.get('wrote_unknown', [])
        for tag, offsets in obs['wrote'].items():
            abs_['wrote'].setdefault(tag, set()).update(offsets)
        for tag, data in obs.get('data', {}).items():
            abs_['data'][tag] = data
        _absorb_return(abs_, obs, ret_kind)
        witness = (record['detail'].get('ikos') or {}).get('witness') or {}
        if witness.get('ok') and isinstance(witness.get('ret'), int):
            abs_.setdefault('witness', []).append(witness['ret'])
    return abs_


def spec_abstraction(records, ret_kind):
    """I^B: the spec abstraction -- what the stub claims, over all alive
    schedule runs of all usable records, joined with what the analyzer
    itself computed for the same inputs (the ikos battery)."""
    abs_ = _blank_abstraction(_kind_of(ret_kind))
    for record in records:
        if not _usable(record):
            continue
        for obs, events in record['detail']['alive']:
            _absorb_return(abs_, obs, ret_kind, events)
            for event in events:
                if event[0] in ('HAVOC_FORGET', 'HAVOC_ABSTRACT') \
                        and event[1] not in ('NULL', 'UNREG'):
                    abs_['wrote'].setdefault(event[1], set()).update(
                        range(event[2], event[2] + event[3]))
                if event[0] == 'ASSUME_VAL' \
                        and event[1] not in ('NULL', 'UNREG'):
                    abs_['content_values'].setdefault(event[1], []).append(
                        (event[2], event[3], event[4], event[5]))
        ik = record['detail'].get('ikos')
        if ik:
            _absorb_ikos(abs_, ik)
    return abs_


#############################
# The potential's terms #
#############################

def _disc_int(real_a, spec_a, width, mode='dual'):
    mode = difftest.as_mode(mode)
    rvals = sorted(set(real_a['values']))
    svals = sorted(set(spec_a['values']))
    if not rvals and not svals:
        return None
    if not svals:
        # the spec claims no measurable return: maximal slack
        return {'component': 'ret-int', 'dist': 1.0,
                'violation': bool(rvals),
                'note': 'no measurable stub return'}
    slo, shi = svals[0], svals[-1]
    # the analyzer-real claimed hull supersedes the concrete-run hull:
    # it covers the whole nondet space, not just the driven schedules
    ikos_note = ''
    if spec_a['ret_ikos']:
        ikos_lo = min(b[0] for b in spec_a['ret_ikos'])
        ikos_hi = max(b[1] for b in spec_a['ret_ikos'])
        if (ikos_lo, ikos_hi) != (slo, shi):
            ikos_note = ' (ikos hull [%d,%d])' % (ikos_lo, ikos_hi)
        slo, shi = ikos_lo, ikos_hi
    swidth = max(shi - slo, 1)
    if not rvals:
        return {'component': 'ret-int', 'dist': 0.0, 'violation': False,
                'note': 'no usable execution data yet'}
    rlo, rhi = rvals[0], rvals[-1]
    dist = min((abs(rlo - slo) + abs(rhi - shi)) / swidth, 1.0)
    need_hull = mode in ('hull', 'dual')
    need_clo = mode in ('closure', 'dual')
    # congruence axis: the observed values carry a gcd structure the spec
    # could express by complete finite enumeration (tighten-congruence) --
    # but only a congruence-tracking target domain can read it
    _r0, rd = difftest._congruence_closure(rvals)
    _s0, sd = difftest._congruence_closure(svals)
    if need_clo and rd > 1 and sd == 1:
        dist = min(dist + 0.5, 1.5)
    # a WIDE hull is driven by nondeterministic arithmetic the battery
    # cannot control, so its congruence closure is not evidence (the same
    # caveat the judge applies)
    wide = shi - slo > (1 << (8 * width)) // 2
    if wide:
        # nothing is conclusive under a closure-only target (the judge
        # returns unverified there); a hull-tracking target still sees the
        # hull misses
        violation = need_hull and any(y < slo or y > shi for y in rvals)
    elif need_hull and need_clo:
        violation = any(y < slo or y > shi
                        or not difftest._in_closure(svals, y)
                        for y in rvals)
    elif need_hull:
        violation = any(y < slo or y > shi for y in rvals)
    else:
        violation = any(not difftest._in_closure(svals, y) for y in rvals)
    violation = violation or spec_a['ret_flag']
    return {'component': 'ret-int', 'dist': dist, 'violation': violation,
            'note': 'hull [%d,%d] vs claims [%d,%d]%s%s'
                    % (rlo, rhi, slo, shi,
                       '' if sd == 1 else
                       ' (claims join to x = %d mod %d)'
                       % (difftest._congruence_closure(svals)[0], sd),
                       ikos_note)}


def _disc_cells(real_a, spec_a):
    """Per-buffer pointee precision, analyzer-computed: how tight is the
    ikos knowledge of each probed cell against the real observed byte.

    dist per cell = min(1, (outside_slack + width/255) / 2): the width
    term keeps a flat [0,255] havoc claim from scoring 0 against a byte
    it merely contains; violation = ikos PROVED this buffer's real byte
    outside the claim (analyzer-real unsoundness signal).
    """
    if not spec_a['cells_ikos']:
        return None
    comps = []
    for tag, cells in sorted(spec_a['cells_ikos'].items()):
        data = real_a['data'].get(tag)
        slacks = []
        for off, (lo, hi) in sorted(cells.items()):
            if data is None or off >= len(data):
                continue
            byte = data[off]
            outside = max(0, lo - byte) + max(0, byte - hi)
            slacks.append(min(1.0, (outside + (hi - lo) / 255.0) / 2.0))
        if not slacks:
            continue
        comps.append({'component': 'cell-%s' % tag,
                      'dist': sum(slacks) / len(slacks),
                      'violation': bool(spec_a['cell_flags'].get(tag)),
                      'note': '%d cell(s), mean slack %.3f (ikos'
                              ' intervals vs real bytes)' % (len(slacks),
                                                             sum(slacks)
                                                             / len(slacks))})
    return comps or None


def _disc_nullity(real_a, spec_a):
    if not spec_a['classes'] and real_a['classes']:
        return {'component': 'ret-nullity', 'dist': 1.0,
                'violation': any(c != 'OTHER' for c in real_a['classes']),
                'note': 'no measurable stub return'}
    rnull = 'NULL' in real_a['classes']
    snull = 'NULL' in spec_a['classes']
    if rnull and not snull:
        return {'component': 'ret-nullity', 'dist': 0.0, 'violation': True,
                'note': 'the real function returned NULL, the stub never can'}
    if snull and not rnull and any(c != 'NULL' for c in real_a['classes']):
        return {'component': 'ret-nullity', 'dist': 1.0, 'violation': False,
                'note': 'the stub keeps a NULL capability never observed'}
    return {'component': 'ret-nullity', 'dist': 0.0, 'violation': False,
            'note': 'nullity matches'}


def _disc_blocks(real_a, spec_a):
    rb = {c for c in real_a['classes'] if c != 'NULL'}
    sb = {c for c in spec_a['classes'] if c != 'NULL'}
    if not rb and not sb:
        return None
    # fresh (OTHER) observations are the stub vocabulary gap the judge
    # covers with sound*; exempt from the violation direction, but they
    # still count toward the symmetric-difference slack
    if rb == {'OTHER'} and sb == {'FRESH'}:
        return {'component': 'ret-blocks', 'dist': 0.0,
                'violation': False,
                'note': 'fresh dynamic allocation (OTHER vs FRESH)'}
    violation = bool({c for c in rb if c != 'OTHER'} - sb)
    union = rb | sb
    dist = len(rb ^ sb) / len(union) if union else 0.0
    return {'component': 'ret-blocks', 'dist': dist, 'violation': violation,
            'note': 'observed blocks %s vs claimed %s'
                    % (sorted(str(c) for c in rb),
                       sorted(str(c) for c in sb))}


def _disc_offsets(real_a, spec_a):
    comps = []
    for tag in sorted(set(real_a['offsets']) & set(spec_a['offsets'])):
        rlo, rhi = real_a['offsets'][tag]
        slo, shi = spec_a['offsets'][tag]
        # mirror the judge's null-join convention: a NULL-capable spec
        # admits offset 0 into the hull
        if 'NULL' in spec_a['classes']:
            slo, shi = min(slo, 0), max(shi, 0)
        width = max(shi - slo, 1)
        dist = min((abs(rlo - slo) + abs(rhi - shi)) / width, 1.0)
        violation = rlo < slo or rhi > shi
        comps.append({'component': 'offset-%s' % tag, 'dist': dist,
                      'violation': violation,
                      'note': 'offsets [%d,%d] vs claimed [%d,%d]'
                              % (rlo, rhi, slo, shi)})
    return comps


def _disc_writes(real_a, spec_a):
    # tags whose real snapshot failed are three-state unknown: not evidence
    unknown = set(real_a['wrote_unknown'])
    comps = []
    for tag in sorted((set(real_a['wrote']) | set(spec_a['wrote'])) - unknown):
        robs = real_a['wrote'].get(tag, set())
        rspec = spec_a['wrote'].get(tag, set())
        union = robs | rspec
        dist = len(union - (robs & rspec)) / len(union) if union else 0.0
        comps.append({'component': 'writes-%s' % tag, 'dist': dist,
                      'violation': bool(robs - rspec),
                      'note': '%d of %d observed bytes outside the havoc'
                              ' claim' % (len(robs - rspec), len(union))})
    return comps


def _disc_content_value(real_a, spec_a):
    """The content-value axis, so the game can see the gap between the real
    written byte values and what the stub claims about them.  When the stub
    claims nothing (forget/abstract = arbitrary), the whole observed range
    is unclaimed slack -- the note tells B to tighten with
    __ikos_assume_mem_value."""
    comps = []
    for tag in sorted(set(real_a['data'])):
        data = real_a['data'].get(tag, b'')
        wrote = real_a['wrote'].get(tag, set())
        obs_vals = [data[o] for o in sorted(wrote) if o < len(data)]
        if not obs_vals:
            continue
        rlo, rhi = min(obs_vals), max(obs_vals)
        claims = spec_a['content_values'].get(tag, [])
        if claims:
            lo = min(c[2] for c in claims)
            hi = max(c[3] for c in claims)
            violation = any(v < lo or v > hi for v in obs_vals)
            if violation:
                dist = 1.0
            else:
                swidth = max(hi - lo, 1)
                dist = min((abs(rlo - lo) + abs(rhi - hi)) / swidth, 1.0)
            note = 'real bytes [%d,%d] vs claimed [%d,%d]' % (rlo, rhi, lo, hi)
        else:
            # no assume_mem_value claim: contents are arbitrary (top)
            dist = 1.0
            violation = False
            note = ('real bytes [%d,%d] unclaimed (forget/abstract = arbitrary;'
                    ' tighten with __ikos_assume_mem_value)' % (rlo, rhi))
        comps.append({'component': 'content-value-%s' % tag, 'dist': dist,
                      'violation': violation, 'note': note})
    return comps


def discrepancy(real_a, spec_a, width=8, mode='dual'):
    """-> {'components': [...], 'd': float, 'inconsistencies': [...]}.

    Each component: {'component', 'dist' (normalized slack), 'violation'
    (an inconsistency signal: observed behavior outside the spec's
    admission), 'note'}.  d is the mean over components of
    dist + VIOLATION_WEIGHT * violation.  mode is difftest.admission_mode()
    of the run's --domain selection: the ret-int violation keeps the same
    scope as the judge (which join families the target domain consults).
    """
    comps = []
    if real_a['kind'] == 'int':
        comps.append(_disc_int(real_a, spec_a, width, mode))
    elif real_a['kind'] == 'pointer':
        comps.append(_disc_nullity(real_a, spec_a))
        comps.append(_disc_blocks(real_a, spec_a))
        comps += _disc_offsets(real_a, spec_a)
    comps += _disc_writes(real_a, spec_a)
    comps += _disc_content_value(real_a, spec_a)
    cells = _disc_cells(real_a, spec_a)
    if cells:
        comps += cells
    comps = [c for c in comps if c is not None]
    d = (sum(c['dist'] + (VIOLATION_WEIGHT if c['violation'] else 0.0)
             for c in comps) / len(comps)) if comps else 0.0
    return {'components': comps, 'd': d,
            'inconsistencies': [c for c in comps if c['violation']]}


def discrepancy_records(records, ret_kind, width=8, mode='dual'):
    """Per-input projection of the discrepancy (relational targets).

    A relational claim is input-dependent: r == |p0| projects to a point
    per input but a wide hull across the pool, so the pooled d of
    discrepancy() cannot see its tightness.  This judges every usable
    record on its own and merges: per component name, dist is the mean
    over the records that produced it and violation is any; d is the
    mean of the per-record d (NOT recomputed from the merged components
    -- an any-merged violation would inflate it by VIOLATION_WEIGHT);
    'inconsistencies' stays the merged any-list so stopping_criterion
    blocks on any single bad input, mirroring the judge's per-input
    semantics.  Honest caveat: a record's spec hull is the battery's
    surviving-run hull, an under-estimate of a wide flat claim -- the
    metric rewards relative tightness directionally (relational d = 0 vs
    flat d > 0 at equal pooled span), it does not measure claim slack
    exactly.
    """
    merged = {}
    ds = []
    count = 0
    for record in records:
        if not _usable(record):
            continue
        count += 1
        real_a = real_abstraction([record], ret_kind)
        spec_a = spec_abstraction([record], ret_kind)
        disc = discrepancy(real_a, spec_a, width, mode)
        ds.append(disc['d'])
        for comp in disc['components']:
            slot = merged.setdefault(comp['component'],
                                     {'dist': 0.0, 'n': 0,
                                      'violation': False, 'note': ''})
            slot['dist'] += comp['dist']
            slot['n'] += 1
            slot['violation'] = slot['violation'] or comp['violation']
            slot['note'] = comp['note']
    if not count:
        return {'components': [], 'd': 0.0, 'inconsistencies': []}
    components = [{'component': name,
                   'dist': slot['dist'] / slot['n'],
                   'violation': slot['violation'],
                   'note': '%s (per-input projection over %d input(s))'
                           % (slot['note'], count)}
                  for name, slot in sorted(merged.items())]
    return {'components': components,
            'd': sum(ds) / len(ds),
            'inconsistencies': [c for c in components if c['violation']]}


def complexity(body):
    """Normalized complexity of a stub body (~[0, 1] for sane bodies).

    __ikos_* helper calls + integer constants + ternary nesting depth, on a
    log scale: simplifying a body always pays, without drowning the return
    terms.
    """
    code = difftest.COMMENT_RE.sub(' ', body)
    calls = len(HELPER_CALL_RE.findall(code))
    constants = len(CONSTANT_RE.findall(code))
    depth = _ternary_depth(code)
    raw = 1 + calls + constants + 2 * depth
    return math.log2(raw) / 8.0


def _ternary_depth(code):
    depth = best = 0
    for char in code:
        if char == '?':
            depth += 1
            best = max(best, depth)
        elif char == ':':
            depth = max(0, depth - 1)
    return best


def coverage(real_a, spec_a):
    """-> (value, faces): the fraction of the spec's claimed faces the
    execution data touches (1.0 when nothing is claimable)."""
    faces = []  # (name, touched)
    if real_a['kind'] == 'int' and spec_a['values'] and real_a['values']:
        svals = sorted(set(spec_a['values']))
        slo, shi = svals[0], svals[-1]
        eps = max(1, (shi - slo) // FACE_WIDTH_DIVISOR)
        rvals = real_a['values']
        faces.append(('ret-lo', any(abs(y - slo) <= eps for y in rvals)))
        faces.append(('ret-hi', any(abs(y - shi) <= eps for y in rvals)))
    elif real_a['kind'] == 'pointer':
        if 'NULL' in spec_a['classes']:
            faces.append(('null', 'NULL' in real_a['classes']))
        for tag in sorted(set(real_a['offsets']) & set(spec_a['offsets'])):
            slo, shi = spec_a['offsets'][tag]
            if 'NULL' in spec_a['classes']:
                slo, shi = min(slo, 0), max(shi, 0)
            eps = max(1, (shi - slo) // FACE_WIDTH_DIVISOR)
            rlo, rhi = real_a['offsets'][tag]
            faces.append(('offset-%s-lo' % tag, abs(rlo - slo) <= eps))
            faces.append(('offset-%s-hi' % tag, abs(rhi - shi) <= eps))
    for tag in sorted(set(real_a['wrote']) & set(spec_a['wrote'])):
        rspec = spec_a['wrote'][tag]
        if not rspec:
            continue
        robs = real_a['wrote'][tag]
        faces.append(('writes-%s-lo' % tag,
                      min(rspec) in robs or min(rspec) + 1 in robs))
        faces.append(('writes-%s-hi' % tag,
                      max(rspec) in robs or max(rspec) - 1 in robs))
    if not faces:
        return 1.0, faces
    return sum(1 for _n, touched in faces if touched) / len(faces), faces


def exploration_gain(prev_real, real_a):
    """First-time discoveries in the execution abstraction (count).

    The measurable form of "inputs moving toward the boundary": new hull
    endpoints, new pointer classes, new write regions.
    """
    if prev_real is None:
        return 0
    gain = 0
    if real_a['kind'] == 'int' and real_a['values']:
        pvals = set(prev_real['values'])
        if pvals:
            plo, phi = min(pvals), max(pvals)
            rlo, rhi = min(real_a['values']), max(real_a['values'])
            gain += (rlo < plo) + (rhi > phi)
        else:
            gain += 1  # the first measurable values
    elif real_a['kind'] == 'pointer':
        gain += len(set(real_a['classes']) - set(prev_real['classes']))
    gain += len(set(real_a['wrote']) - set(prev_real['wrote']))
    return int(gain)


def potential(d_value, complexity_value, coverage_value, opt):
    """Phi = d + lambda * Complexity - mu * Coverage (smaller is better)."""
    return (d_value + _co(opt, 'lambda') * complexity_value
            - _co(opt, 'mu') * coverage_value)


def payoff_a(delta_phi, exploration, opt):
    return delta_phi + _co(opt, 'alpha') * exploration


def payoff_b(delta_phi, complexity_delta, opt):
    return delta_phi - _co(opt, 'beta') * max(0.0, complexity_delta)


#############################
# Stopping / equilibrium #
#############################

def stopping_criterion(history, opt):
    """-> None | 'potential-stable' | 'empirical-nash'.

    Both criteria are gated on the last two settlements covering one stage
    by each player (an 'A' entry and a 'B' entry, in either order): the
    game only stops after a full asynchronous cycle in which neither
    player improved -- never right after a single player's move.  Potential
    stability: |Delta-Phi| < epsilon across that cycle.  Empirical
    epsilon-Nash (honest wording: over the REALIZED action set, not all
    strategies): both players' current payoffs are within epsilon of zero
    AND within epsilon of their historical best -- no realized deviation
    would have paid meaningfully more.
    """
    epsilon = _co(opt, 'epsilon')
    if len(history) < 2:
        return None
    if history[-1]['inconsistencies']:
        return None  # an open inconsistency must be answered, not settled
    if {h.get('stage') for h in history[-2:]} != {'A', 'B'}:
        return None  # one player has not settled a move since the last check
    if all(abs(h['delta_phi']) < epsilon for h in history[-2:]):
        return 'potential-stable'
    best_a = max(h['u_a'] for h in history)
    best_b = max(h['u_b'] for h in history)
    last = history[-1]
    if (abs(last['u_a']) < epsilon and abs(last['u_b']) < epsilon
            and best_a - last['u_a'] < epsilon
            and best_b - last['u_b'] < epsilon):
        return 'empirical-nash'
    return None


def verify_equilibrium(kit, opt, plan, parts, driver_path, pool, stub_src,
                       rundir, info=None):
    """Deterministic boundary battery run before declaring an equilibrium.

    Anti-collusion defense: zero LLM cost, no incentive involved.  Re-plays
    edge-token variants of the pool's recent argv shapes against the current
    body; -> the inputs that produced an inconsistency signal ([] when the
    equilibrium holds, which the caller then trusts).
    """
    battery = [dict(entry) for entry in difftest._edge_inputs(pool[-8:])]
    seen = {tuple(entry['argv']) for entry in pool}
    battery = [entry for entry in battery
               if tuple(entry['argv']) not in seen]
    if not battery:
        return []
    records, run_err = difftest.run_battery(kit, opt, plan, parts,
                                            driver_path, battery, stub_src,
                                            rundir, info)
    if run_err or not records:
        raise RuntimeError(run_err or 'the equilibrium battery produced'
                           ' no records')
    return [entry for entry, record in zip(battery, records)
            if record['verdict'] == 'unsound']


#############################
# Player prompts #
#############################

A_SYSTEM_PROMPT = """You are the execution player (A) in a two-agent co-evolutionary game \
about the IKOS stub of one library function.  Player B (the invariant \
agent) writes the stub body; you generate the execution inputs that \
produce the execution data the game measures.

Your payoff each move:  u_A = Delta-Phi + alpha * ExplorationBonus
  Phi = d + lambda * Complexity(stub) - mu * Coverage(inputs)
  d   = normalized distance between the execution abstraction (what the \
real function did on your inputs) and the spec abstraction (what the stub \
claims): return hull boundary difference, congruence axis, nullity, block \
set, per-block offsets, write sets.
In the default game alpha = 0: every point of your payoff is Phi made \
smaller.  ExplorationBonus is then not a payoff term but your tie-break \
heuristic -- between equally promising moves, prefer the one reaching \
what no input has reached yet (a first-time hull endpoint, pointer class \
or write region is where the next Delta-Phi hides).

A SMALLER Phi is a better game state.  You lower d by pushing the \
execution data toward the boundaries of B's claims: claims that are too \
narrow turn into inconsistency signals that force B to widen; claims that \
are slack get exposed where they exceed the evidence.  You raise Coverage \
by touching claimed faces (hull endpoints, NULL, extreme offsets, write \
edges).

Strategy menu -- pick the one with the best expected payoff given the \
component table and the score history:
  targeted-boundary    aim at the component with the largest distance or an \
untouched claimed face
  distribution-shift   change the value distribution (extreme / empty / mixed)
  shape-variant        a new argv shape the driver accepts
  replay-mutate        mutate inputs from the history that moved d
  deterministic-sweep  boundary values (0, -1, 1, type extremes)

Reply with ONLY a JSON object, no fences, no explanations:
  {"strategy": "<menu id>", "inputs": [{"argv": ["token", ...]}, ...]}
Tokens follow the driver's argv mapping.  You produce inputs, never code."""


B_SYSTEM_PROMPT = """You are the invariant player (B) in a two-agent co-evolutionary game \
about the IKOS stub of one library function.  Your invariant IS the stub \
body (__ikos_* helper calls): it over-approximates the real function \
during static analysis.  Player A generates execution inputs; the harness \
measures the distance between the execution data and your claims.

Your payoff each move:  u_B = Delta-Phi - beta * ComplexityPenalty
  Phi = d + lambda * Complexity(stub) - mu * Coverage(inputs)

A SMALLER Phi is a better game state.  You lower d by removing slack: \
tighten hulls toward the observed execution data, drop unobserved \
capabilities -- but never below the evidence, or the components turn into \
inconsistency signals, you are forced to widen again, and the pipeline \
rejects the stub.  Removing helpers and constants also lowers Complexity.

Acceptance rule: the harness lets a revision STAY only when its settled \
payoff u_B exceeds the threshold tau -- by default that means the \
revision must not worsen Phi beyond the game's tolerance, and resolving \
a standing inconsistency signal always qualifies (its payoff dwarfs the \
threshold).  A revision that creates an inconsistency signal prices far \
below the threshold and is always reverted.  A rejected move is reverted \
to the standing body and appears in the history marked -rejected with \
the reason; answer it with a better move, not a repetition.

Strategy menu:
  tighten-interval        narrow a return / offset hull toward the evidence
  tighten-congruence      express value structure as a COMPLETE finite \
enumeration; {v1, v2, ...} with gcd of differences 1 admits everything, \
an incomplete enumeration is unsound under the congruence domains
  shrink-havoc            narrow a havoc claim toward the observed writes
  tighten-content-value   narrow the [lo,hi] of __ikos_assume_mem_value toward the evidence
  drop-null-capability    remove a NULL capability never observed
  simplify                remove redundant helpers / constants
  widen-on-inconsistency  REQUIRED after an inconsistency signal: widen the \
offending claim until it covers the evidence

Available stub helpers (exactly these, already declared):
  __ikos_assert(cond)  precondition to prove at call sites
  __ikos_assume(cond)  postcondition; false paths are pruned
  __ikos_nondet_int() / __ikos_nondet_uint()  nondeterministic values
  __ikos_malloc(n)  fresh n-byte allocation (malloc model); use this for \
allocator-like APIs, never an integer-to-pointer cast
  __ikos_check_mem_access(p, n)  require [p, p+n) accessible
  __ikos_check_string_access(s)  require a valid NUL-terminated string
  __ikos_assume_mem_size(p, n)  assume the memory block length
  __ikos_forget_mem(p, n)  arbitrary bytes, possibly valid pointers
  __ikos_abstract_mem(p, n)  arbitrary bytes, no valid pointers
  __ikos_assume_mem_value(p, n, lo, hi)  bounded integer contents
  __ikos_assume_mem_rel(p1, n1, p2, n2, kind)  pure refinement of \
corresponding 8-byte signed chunks, without havoc; kind 0 means ==, 1 means <=, \
2 means >=; for newly written outputs, havoc both buffers first, then add \
the API-guaranteed integer-chunk relation; never use it for opaque structs \
or pointer identity
  __ikos_watch_mem(p, n)  debugger watch only; it does not havoc memory \
or model a side effect

Hard rules (unchanged from stub writing): C99; only <stddef.h> identifiers \
and the __ikos_* helpers; only the parameters p0, p1, ...; over-approximate \
always; never cast a nondeterministic value to a pointer.  A numeric \
return narrowed to a finite set must contain EVERY value the real function \
can return.  Do not remove __ikos_malloc from an allocator-like stub: the \
fresh dynamic-allocation block is part of the API semantics.

Reply with ONLY a JSON object, no fences, no explanations:
  {"strategy": "<menu id>", "body": "<the body statements, \\n-separated>"}
The body replaces the current one verbatim: body statements only, no \
signature, no braces around the whole body."""

# appended to B_SYSTEM_PROMPT under a relational --domain target (the
# default prompt stays byte-identical; CO_PROMPT_VERSION tracks the menu)
_B_RELATIONAL_BLOCK = """
Under this game's target numeric domain the analyzer narrows two-variable \
__ikos_assume predicates, so an extra strategy pays:
  tighten-relational     constrain the return RELATIVE to a numeric \
parameter instead of a flat range: a corridor (assume(r <= p0 + 16); \
assume(r >= p0 - 16);) or, piecewise, a branch on the parameter with an \
exact equality per arm.  The claim must still contain every value the \
real function returns on every input; relate only to numeric parameters, \
never to a dereference or a pointer parameter."""


def b_system_prompt(domain='all'):
    """B's system prompt under a --domain selection.

    The default returns B_SYSTEM_PROMPT byte for byte; a relational
    backend (difftest.relation_class()) splices the tighten-relational
    strategy block in after the strategy menu (the 'Hard rules' anchor;
    single-count guard, plain-append fallback).
    """
    if difftest.relation_class(domain) is None:
        return B_SYSTEM_PROMPT
    anchor = 'Hard rules'
    if B_SYSTEM_PROMPT.count(anchor) != 1:
        return B_SYSTEM_PROMPT + _B_RELATIONAL_BLOCK
    head, tail = B_SYSTEM_PROMPT.split(anchor, 1)
    return head + _B_RELATIONAL_BLOCK.strip('\n') + '\n\n' + anchor + tail


def _fmt_component_table(disc):
    lines = ['component | distance | inconsistency | detail']
    for c in disc['components']:
        lines.append('%s | %.3f | %s | %s'
                     % (c['component'], c['dist'],
                        'YES' if c['violation'] else '-', c['note']))
    return '\n'.join(lines) if len(lines) > 1 else '(no measurable components)'


def _fmt_history(history):
    if not history:
        return '(first round: no history yet)'
    lines = []
    for h in history[-5:]:
        lines.append('round %d %-8s a=%-18s u_A=%+.3f | b=%-22s u_B=%+.3f'
                     ' | Phi=%.3f dPhi=%+.3f d=%.3f'
                     % (h['round'], '(%s)' % h.get('stage', '-'),
                        h['a_strategy'], h['u_a'],
                        h['b_strategy'], h['u_b'], h['phi'],
                        h['delta_phi'], h['d']))
    return '\n'.join(lines)


def _fmt_data(real_a):
    if real_a['kind'] == 'int':
        return 'return values %s; wrote tags %s' % (
            sorted(set(real_a['values']))[:16],
            {tag: [min(o), max(o)] if o else None
             for tag, o in sorted(real_a['wrote'].items())})
    if real_a['kind'] == 'pointer':
        return ('pointer classes %s; offsets %s; wrote tags %s'
                % (sorted(str(c) for c in real_a['classes']),
                   {tag: list(hi_lo)
                    for tag, hi_lo in sorted(real_a['offsets'].items())},
                   {tag: [min(o), max(o)] if o else None
                    for tag, o in sorted(real_a['wrote'].items())}))
    return 'wrote tags %s' % {tag: [min(o), max(o)] if o else None
                              for tag, o in sorted(real_a['wrote'].items())}


def _domain_line(domain):
    """One line telling both players which join semantics judge claims."""
    mode = difftest.admission_mode(domain)
    how = {'hull': 'the convex hull only',
           'closure': 'the congruence class only',
           'dual': 'both the convex hull and the congruence class'}[mode]
    rel = difftest.relation_class(domain)
    if rel is not None:
        what = {'difference': 'difference constraints r - p <= c',
                'polyhedra': 'linear constraints over several variables',
                'equalities': 'linear equalities'}[rel]
        return ('Target numeric domain: %s -- the judge admits a claim by'
                ' %s (per-input projection); the analyzer narrows %s, so'
                ' relational claims pay.'
                % (domain, how, what))
    if domain == 'all':
        how += ' (portable across every -d backend)'
    return ('Target numeric domain: %s -- the judge admits a claim by %s.'
            % (domain, how))


def attack_user_prompt(plan, rnd, real_a, spec_a, disc, cov, history, batch,
                       driver_src, domain='all'):
    lines = ['Game round %d for function %s.'
             % (rnd, plan.source_name), '']
    lines.append(_domain_line(domain))
    lines.append('')
    lines.append('Phi decomposition: d = %.3f (interval discrepancy plus'
                 ' inconsistency signals), coverage = %.2f.'
                 % (disc['d'], cov[0]))
    untouched = [name for name, touched in cov[1] if not touched]
    if untouched:
        lines.append('Claimed faces NOT touched by the execution data: %s'
                     ' (touching them raises Coverage, which lowers Phi).'
                     % ', '.join(untouched))
    lines.append('')
    lines.append('Component table (execution abstraction vs stub claims):')
    lines.append(_fmt_component_table(disc))
    lines.append('')
    lines.append('Execution data so far: %s' % _fmt_data(real_a))
    if disc['inconsistencies']:
        lines.append('')
        lines.append('Inconsistency signals stand on: %s (B must widen;'
                     ' replaying that evidence consolidates the rejection).'
                     % ', '.join(c['component']
                                 for c in disc['inconsistencies']))
    lines.append('')
    lines.append('Recent rounds:')
    lines.append(_fmt_history(history))
    lines.append('')
    lines.append('The driver (how argv tokens map to arguments):')
    lines.append(driver_src)
    lines.append('')
    lines.append('You move first this round; B revises the body only after'
                 ' the battery settles your inputs.  Propose %d new inputs'
                 ' as your next move.' % batch)
    return '\n'.join(lines)


def spec_user_prompt(plan, rnd, body, real_a, disc, comp_n, cov, history,
                     domain='all'):
    lines = ['Game round %d for function %s.'
             % (rnd, plan.source_name), '']
    lines.append(_domain_line(domain))
    lines.append('')
    lines.append('Your current invariant (stub body):')
    lines.append(body)
    lines.append('')
    lines.append('Phi decomposition: d = %.3f, Complexity = %.3f,'
                 ' coverage = %.2f.' % (disc['d'], comp_n, cov[0]))
    lines.append('Component table (where your claims stand against the'
                 ' evidence):')
    lines.append(_fmt_component_table(disc))
    lines.append('')
    lines.append('Execution data: %s' % _fmt_data(real_a))
    if disc['inconsistencies']:
        lines.append('')
        lines.append('INCONSISTENCY SIGNALS -- your claims exclude observed'
                     ' behavior; widen them (widen-on-inconsistency):')
        for c in disc['inconsistencies']:
            lines.append('  %s: %s' % (c['component'], c['note']))
    lines.append('')
    lines.append('Recent rounds:')
    lines.append(_fmt_history(history))
    lines.append('')
    lines.append('You move after A this round: the execution data and the'
                 ' component table above already include the inputs A added'
                 ' this round.  Play your next move.')
    return '\n'.join(lines)


#############################
# Moves #
#############################

def _pick_strategy(policy, menu, fixed):
    """The ablation arms: greedy = the LLM optimizes; random / fixed = the
    harness dictates the strategy (RQ1/RQ2 controls)."""
    if policy == 'random':
        return random.choice(menu)
    if policy == 'fixed':
        return fixed
    return None


def _parse_attack(content):
    """-> (strategy or None, inputs list)."""
    text = difftest._strip_fences(content.strip())
    data = None
    try:
        data = json.loads(text)
    except ValueError:
        start, end = text.find('{'), text.rfind('}')
        if start >= 0 and end > start:
            try:
                data = json.loads(text[start:end + 1])
            except ValueError:
                data = None
    strategy = None
    entries = None
    if isinstance(data, dict):
        if isinstance(data.get('strategy'), str):
            strategy = data['strategy']
        entries = data.get('inputs')
    if not isinstance(entries, list):
        entries = difftest._parse_inputs(text, 16)
    inputs = []
    seen = set()
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        argv = entry.get('argv')
        if not isinstance(argv, list) or not argv:
            continue
        argv = tuple(str(t) for t in argv
                     if isinstance(t, (str, int, float)))
        if argv and argv not in seen:
            seen.add(argv)
            inputs.append({'argv': list(argv), 'stdin': ''})
    return strategy, inputs


def _parse_spec(content):
    """-> (strategy or None, body).  A non-JSON reply is treated as a bare
    body (the round-0 output contract)."""
    text = difftest._strip_fences(content.strip())
    try:
        data = json.loads(text)
        if isinstance(data, dict) and isinstance(data.get('body'), str):
            strategy = data.get('strategy')
            return (strategy if isinstance(strategy, str) else None,
                    data['body'])
    except ValueError:
        pass
    return None, text


def attacker_move(complete, opt, plan, rnd, real_a, spec_a, disc, cov,
                  history, driver_src):
    """-> (strategy, inputs)."""
    forced = _pick_strategy(_co(opt, 'a_policy'), A_STRATEGIES,
                            A_FIXED_STRATEGY)
    user = attack_user_prompt(plan, rnd, real_a, spec_a, disc, cov, history,
                              _co(opt, 'attack_batch'), driver_src,
                              getattr(opt, 'domain', 'all'))
    if _co(opt, 'alpha') > 0:
        # the delta-approximate ablation arm announces its perturbation;
        # the default game (alpha = 0) keeps the payoff purely Delta-Phi
        user += ('\n\nExplorationBonus is active this game (alpha = %.2f):'
                 ' first-time discoveries pay on top of Delta-Phi.'
                 % _co(opt, 'alpha'))
    if forced:
        user += ('\n\nYour strategy this round is set by the harness: %s.'
                 ' Execute exactly that strategy.' % forced)
    content, _usage = complete([{'role': 'system', 'content': A_SYSTEM_PROMPT},
                                {'role': 'user', 'content': user}])
    strategy, inputs = _parse_attack(content)
    if forced:
        strategy = forced
    return strategy or 'auto', inputs[:_co(opt, 'attack_batch')]


def speculator_move(complete, opt, plan, rnd, body, real_a, disc, comp_n,
                    cov, history, error_note=None):
    """-> (strategy, new body text)."""
    forced = _pick_strategy(_co(opt, 'b_policy'), B_STRATEGIES,
                            B_FIXED_STRATEGY)
    user = spec_user_prompt(plan, rnd, body, real_a, disc, comp_n, cov,
                            history, getattr(opt, 'domain', 'all'))
    if _co(opt, 'beta') > 0:
        # the delta-approximate ablation arm announces its perturbation;
        # the default game (beta = 0) keeps the payoff purely Delta-Phi
        user += ('\n\nComplexityPenalty is active this game (beta = %.2f):'
                 ' complexity increases are subtracted from your payoff.'
                 % _co(opt, 'beta'))
    if forced:
        user += ('\n\nYour strategy this round is set by the harness: %s.'
                 ' Execute exactly that strategy.' % forced)
    if error_note:
        user += ('\n\nYour last move was reverted by the harness:\n%s\n'
                 'Return a revised body.' % error_note)
    content, _usage = complete([{'role': 'system',
                                 'content': b_system_prompt(
                                     getattr(opt, 'domain', 'all'))},
                                {'role': 'user', 'content': user}])
    strategy, new_body = _parse_spec(content)
    if forced:
        strategy = forced
    return strategy or 'auto', new_body


#############################
# The gate #
#############################

def gate(kit, opt, plan, info, parts, complete, cache, cand_src, body,
         materialize):
    """Potential-game gate around the difftest battery.

    -> (reason, summary, tokens, final_body, reliable_body).  reason
    follows the difftest.check_stub contract (a counterexample text for the
    repair loop when the FINAL body is provably not an over-approximation;
    None when it passed or the harness could not run).  final_body is the
    game's last body when it differs from the input body -- the caller
    installs and caches that one.  reliable_body is the game's reliable
    floor: the newest body that passed the whole battery on the pool as it
    then stood, re-verified on the final input pool, for the caller to
    install if every repair round fails -- a bounded number of iterations
    then still returns a sound stub.  ``--co-rounds 0`` degrades to the
    single-shot difftest gate unchanged (no floor: no game means no
    in-game witness).  With ``--co-score-snapshot``, the zero-round path
    instead settles the final candidate once with d/Phi/coverage, but
    makes no player move and never revises the body.

    ``materialize(body) -> src_path`` splices a revised body into the
    candidate file (raising ValueError when it violates the lexical
    constraints) so this module never imports the splicer.
    """
    if int(_co(opt, 'rounds')) <= 0 \
            and not getattr(opt, 'co_score_snapshot', False):
        reason, summary = difftest.check_stub(kit, opt, plan, info, parts,
                                              complete, cache, cand_src)
        return reason, summary, 0, None, None
    log = kit.log
    spent = [0]

    def counting(messages):
        content, usage = complete(messages)
        spent[0] += usage.get('total_tokens') or 0
        return content, usage

    workdir = os.path.join(os.path.expanduser(opt.cache_dir), 'coevolve',
                           kit.stub.stub_key(plan.name))
    os.makedirs(workdir, exist_ok=True)
    rundir = tempfile.mkdtemp(prefix='ikos-co-')
    try:
        return _play(kit, opt, plan, info, parts, counting, cache, cand_src,
                     body, materialize, workdir, rundir, log, spent)
    finally:
        if getattr(opt, 'dt_keep_temps', False):
            log.info('%s: coevolve scratch kept in %s' % (plan.name, rundir))
        else:
            shutil.rmtree(rundir, ignore_errors=True)


def _play(kit, opt, plan, info, parts, complete, cache, cand_src, body,
          materialize, workdir, rundir, log, spent):
    orig_body = body
    driver_path, inputs, error = difftest._obtain_artifacts(
        opt, plan, info, parts, complete, cache, rundir,
        kit.settings.clang())
    if error:
        log.warning('%s: co-evolution gate skipped: %s' % (plan.name, error))
        return None, {'status': 'harness-error', 'fn': plan.name,
                      'detail': error}, spent[0], None, None
    with open(driver_path) as stream:
        driver_src = stream.read()
    ret_type, _name = difftest.split_signature(parts.signature_line)
    ret_kind = difftest.classify_return(ret_type or 'int')
    width = ret_kind[1] if isinstance(ret_kind, tuple) else 8
    mode = difftest.admission_mode(getattr(opt, 'domain', 'all'))
    # a relational target judges the spec input by input: the pooled
    # abstraction stays for coverage/exploration/prompts/game-state
    relational = difftest.relation_class(
        getattr(opt, 'domain', 'all')) is not None

    def judge(pool, src):
        return difftest.run_battery(kit, opt, plan, parts, driver_path,
                                    pool, src, rundir, info)

    # the reliable floor: every body that passed the whole pool as it
    # stood, oldest to newest (consecutive repeats collapsed).  The pool
    # only grows, so a pass on an earlier pool is re-verified on the final
    # one before any floor is handed back (see the final settlement).
    witnesses = []

    def note_witness(records, cur_body, cur_src):
        if not any(r['verdict'] == 'unsound' for r in records):
            if not witnesses or witnesses[-1][0] != cur_body:
                witnesses.append((cur_body, cur_src))

    pool = [dict(entry) for entry in inputs]
    seen_argv = {tuple(entry['argv']) for entry in pool}
    stub_src = cand_src
    records, run_err = judge(pool, stub_src)
    if run_err:
        log.warning('%s: co-evolution gate skipped: %s'
                    % (plan.name, run_err))
        return None, {'status': 'harness-error', 'fn': plan.name,
                      'detail': run_err}, spent[0], None, None
    note_witness(records, body, stub_src)

    # a body that already fails the battery is a repair-loop matter, not a
    # precision matter: the game only starts from a sound candidate
    early = next((r for r in records if r['verdict'] == 'unsound'), None)
    if early is not None:
        reason = difftest._counterexample(plan, early['entry'],
                                          early['detail']['real'],
                                          early['reason'])
        return reason, {'status': 'unsound', 'fn': plan.name,
                        'stopped': 'round-0',
                        'counts': _verdict_counts(records)}, spent[0], None, \
            None

    history = []
    strat_stats = {'A': {}, 'B': {}}
    prev = None  # (phi, complexity, real_abstraction) of the last settlement
    a_strategy = b_strategy = 'round-0'
    stopped = ('score-snapshot'
               if getattr(opt, 'co_score_snapshot', False) else 'budget')
    vetoes = 0
    error_note = None
    rounds = int(_co(opt, 'rounds'))
    # the acceptance threshold: an unset tau derives to -epsilon, so by
    # default a revision stays when it does not worsen Phi beyond the
    # equilibrium tolerance (an epsilon-tolerant better response: the
    # battery-sampled alive hull cannot see every claim tightening, and
    # strict acceptance would freeze B on metric-invisible revisions);
    # tau = 0 is the strict arm.  w_v's range domination keeps any
    # revision that creates an inconsistency signal far below both.
    tau = _co(opt, 'tau')
    if tau is None:
        tau = -_co(opt, 'epsilon')
        # game.json's coefficients record the threshold actually used
        opt.co_tau = tau
    # the battery is deterministic, so a stage that changed nothing (A
    # proposed no new input, B's body came back unchanged) reuses the
    # standing records instead of re-running identical builds
    dirty = False
    # the settled view the next mover plays on:
    # (real_a, spec_a, disc, comp_n, cov, faces)
    settled = [None]

    def measure(recs, cur_body):
        """Every metric of one candidate settlement ((records, body)
        against the last settlement) -- the single pricing formula shared
        by the acceptance rule and settle(), so what a move is priced at
        is exactly what it would settle with."""
        real_a = real_abstraction(recs, ret_kind)
        spec_a = spec_abstraction(recs, ret_kind)
        disc = (discrepancy_records(recs, ret_kind, width, mode)
                if relational
                else discrepancy(real_a, spec_a, width, mode))
        comp_n = complexity(cur_body)
        cov, faces = coverage(real_a, spec_a)
        phi = potential(disc['d'], comp_n, cov, opt)
        explo = exploration_gain(prev[2] if prev else None, real_a)
        delta_phi = (prev[0] - phi) if prev else 0.0
        return {'real_a': real_a, 'spec_a': spec_a, 'disc': disc,
                'comp_n': comp_n, 'cov': cov, 'faces': faces, 'phi': phi,
                'explo': explo, 'delta_phi': delta_phi,
                'u_a': payoff_a(delta_phi, explo, opt),
                'u_b': payoff_b(delta_phi,
                                (comp_n - prev[1]) if prev else 0.0, opt)}

    def settle(rnd, stage, mover):
        """One settlement of the current (pool, body) -> a history entry,
        and the settled view the next mover plays on.  mover is the
        stage's moving player ('A' / 'B', whose realized payoff lands in
        the strategy table) or None (the round-0 start and the
        deterministic battery's veto evidence: no player move to credit)."""
        nonlocal prev
        m = measure(records, body)
        history.append({'round': rnd, 'stage': stage, 'phi': m['phi'],
                        'd': m['disc']['d'], 'delta_phi': m['delta_phi'],
                        'complexity': m['comp_n'], 'coverage': m['cov'],
                        'u_a': m['u_a'], 'u_b': m['u_b'],
                        'exploration': m['explo'],
                        'a_strategy': a_strategy, 'b_strategy': b_strategy,
                        'pool_size': len(pool),
                        'components': m['disc']['components'],
                        'inconsistencies': [c['component']
                                            for c
                                            in m['disc']['inconsistencies']],
                        'verdict_counts': _verdict_counts(records)})
        if mover == 'A':
            _record_strategy(strat_stats['A'], a_strategy, m['u_a'])
        elif mover == 'B':
            _record_strategy(strat_stats['B'], b_strategy, m['u_b'])
        inc = history[-1]['inconsistencies']
        log.info('%s: game round %d (%s): Phi=%.3f d=%.3f cov=%.2f%s'
                 % (plan.name, rnd, stage, m['phi'], m['disc']['d'],
                    m['cov'],
                    ' INCONSISTENCY: %s' % ','.join(inc) if inc else ''))
        prev = (m['phi'], m['comp_n'], m['real_a'])
        return (m['real_a'], m['spec_a'], m['disc'], m['comp_n'], m['cov'],
                m['faces'])

    def try_stop(rnd):
        """Stopping check + equilibrium battery on the last settlement.

        -> 'stop' (equilibrium accepted, or the re-judge failed and the
        game ends fail-closed), 'veto' (equilibrium falsified: the battery
        inputs joined the pool, their settlement was appended, and the veto
        consumed this round's budget), or None (play on)."""
        nonlocal stopped, vetoes, records, run_err, dirty, a_strategy
        stop = stopping_criterion(history, opt)
        if stop is None:
            return None
        try:
            broken = verify_equilibrium(kit, opt, plan, parts, driver_path,
                                        pool, stub_src, rundir, info)
        except RuntimeError as error:
            run_err = str(error)
            stopped = 'harness-error'
            return 'stop'
        if not broken:
            stopped = stop
            return 'stop'
        # equilibrium falsified: the battery inputs join the pool as
        # evidence and the game continues (a veto costs budget)
        vetoes += 1
        log.info('%s: game round %d: equilibrium VETOED by the'
                 ' deterministic battery (%d input(s))'
                 % (plan.name, rnd, len(broken)))
        pool.extend(broken)
        seen_argv.update(tuple(entry['argv']) for entry in broken)
        records, run_err = judge(pool, stub_src)
        if run_err:
            stopped = 'budget'
            return 'stop'
        note_witness(records, body, stub_src)
        dirty = False
        a_strategy = 'deterministic-sweep'
        settled[0] = settle(rnd, 'battery', None)
        return 'veto'

    # round 0 settles the starting state: the pipeline's own artifacts
    # (the generated body, the gate's driver and inputs) before anyone moves
    settled[0] = settle(1, 'start', None)

    # alternating moves: A attacks -> the battery settles -> B revises the
    # body -> the battery settles.  Each player moves on the other's
    # freshest settled evidence; the idle player's payoff is exactly its
    # Delta-Phi share of the move (its perturbation term is zero because
    # nothing it owns changed).  This asynchronous schedule is the dynamic
    # under which an exact potential game's FIP guarantees convergence --
    # the simultaneous update FIP says nothing about never runs.
    for rnd in range(1, rounds + 1):
        # ---- A's stage: new inputs, then the settlement they earn ----
        # evidence is never subject to the acceptance rule: an input that
        # exposes an inconsistency RAISES Phi yet is the most valuable
        # move in the game, and dropping it would hide unsoundness
        real_a, spec_a, disc, comp_n, cov, faces = settled[0]
        a_strategy, attacks = attacker_move(complete, opt, plan, rnd,
                                            real_a, spec_a, disc,
                                            (cov, faces), history,
                                            driver_src)
        for entry in attacks:
            key = tuple(entry['argv'])
            if key not in seen_argv:
                seen_argv.add(key)
                pool.append(entry)
                dirty = True
        if dirty:
            records, run_err = judge(pool, stub_src)
            if run_err:
                break
            note_witness(records, body, stub_src)
            dirty = False
        settled[0] = settle(rnd, 'A', 'A')
        outcome = try_stop(rnd)
        if outcome == 'stop':
            break
        if outcome == 'veto':
            continue  # the veto consumed this round's budget

        # ---- B's stage: revise the body on A's same-round settlement ----
        real_a, spec_a, disc, comp_n, cov, faces = settled[0]
        b_strategy, new_body = speculator_move(complete, opt, plan, rnd,
                                               body, real_a, disc, comp_n,
                                               (cov, faces), history,
                                               error_note)
        error_note = None
        prev_body = body
        if new_body and new_body.strip() and new_body != body:
            try:
                stub_src = materialize(new_body)
                body = new_body
                dirty = True
            except ValueError as why:
                error_note = 'the lexical guard rejected the body: %s' % why
                b_strategy += '-invalid'
        if dirty:
            standing = records  # invariant: records <-> (pool, standing body)
            records, run_err = judge(pool, stub_src)
            if run_err and body != prev_body:
                # the move broke the build: restore the last judged body ON
                # DISK (materialize overwrites the candidate file in place)
                # and feed the compiler output back as the invalid-move note
                log.warning('%s: game round %d: the revised body failed to'
                            ' build; reverting' % (plan.name, rnd))
                error_note = 'the body failed to compile:\n%s' % run_err
                body = prev_body
                stub_src = materialize(prev_body)
                records, run_err = judge(pool, stub_src)
            if run_err:
                break
            if body != prev_body:
                # tau acceptance: a revision may only settle to stay when
                # its priced u_B exceeds tau (default -epsilon: an
                # epsilon-tolerant better response).  A revision that
                # pushes an observed value out of its claim weighs
                # VIOLATION_WEIGHT -- above the dynamic range of every
                # non-violation term -- so no tightening elsewhere can
                # ever pay for the inconsistency signal it causes: it
                # prices far below tau and is always reverted
                candidate = measure(records, body)
                if candidate['u_b'] > tau:
                    note_witness(records, body, stub_src)
                else:
                    body = prev_body
                    stub_src = materialize(prev_body)
                    # the battery is deterministic, so the standing records
                    # ARE the reverted state's records
                    records = standing
                    error_note = (
                        'your move was reverted by the acceptance rule:'
                        ' u_B = %+.3f did not exceed tau = %.3f; a'
                        ' revision that worsens Phi beyond the tolerance'
                        ' -- or creates an inconsistency signal -- may'
                        ' not stay; resolve the signal or improve Phi'
                        % (candidate['u_b'], tau))
                    b_strategy += '-rejected'
                    log.info('%s: game round %d: revision rejected'
                             ' (u_B=%+.3f <= tau=%.3f); body reverted'
                             % (plan.name, rnd, candidate['u_b'], tau))
            else:
                note_witness(records, body, stub_src)
            dirty = False
        settled[0] = settle(rnd, 'B', 'B')
        if try_stop(rnd) == 'stop':
            break
        # a veto here consumed the round; the next round starts fresh

    # ---- final settlement ----
    if stopped == 'harness-error':
        detail = run_err or 'the equilibrium differential battery failed'
        log.warning('%s: co-evolution gate skipped: %s'
                    % (plan.name, detail))
        return None, {'status': 'harness-error', 'fn': plan.name,
                      'detail': detail}, spent[0], None, None
    if not records:
        log.warning('%s: co-evolution gate skipped: %s'
                    % (plan.name, run_err or 'battery produced no records'))
        return None, {'status': 'harness-error', 'fn': plan.name,
                      'detail': run_err or 'battery produced no records'}, \
            spent[0], None, None
    reason = None
    for record in records:
        if record['verdict'] == 'unsound':
            reason = difftest._counterexample(plan, record['entry'],
                                              record['detail']['real'],
                                              record['reason'])
            break
    reliable_body = None
    if reason is not None and witnesses:
        # budget exhausted holding an unsound body: the reliable floor is
        # the NEWEST witness that survives the FINAL pool -- the pool only
        # grew, so every earlier pass is re-verified before it counts
        # (the newest first: it is the tightest sound state the game had)
        for w_body, _w_src in reversed(witnesses):
            w_records, w_err = judge(pool, materialize(w_body))
            if w_err:
                break  # the harness itself failed: no floor is provable
            if not any(r['verdict'] == 'unsound' for r in w_records):
                reliable_body = w_body
                log.info('%s: reliable floor re-verified on the final'
                         ' pool (%d input(s))' % (plan.name, len(pool)))
                break
            log.info('%s: witness failed re-verification on the final'
                     ' pool; trying the previous one' % plan.name)
    counts = _verdict_counts(records)
    game = _game_state(plan, opt, history, strat_stats,
                       real_abstraction(records, ret_kind),
                       spec_abstraction(records, ret_kind), body, stopped,
                       vetoes, reliable_body)
    _checkpoint(workdir, game, opt)
    summary = {'status': 'unsound' if reason else 'pass', 'fn': plan.name,
               'stopped': stopped, 'rounds': len(history), 'vetoes': vetoes,
               'counts': counts,
               'phi_curve': [h['phi'] for h in history],
               'counts_line': ('unsoundness FOUND on %d input(s): %s'
                               % (counts.get('unsound', 0),
                                  _counts_line(counts)) if reason else
                               'no unsoundness found on %d input(s): %s'
                               % (len(records), _counts_line(counts)))}
    if reason is None:
        log.info('%s: co-evolution game ended (%s): Phi curve %s'
                 % (plan.name, stopped,
                    ['%.3f' % v for v in summary['phi_curve']]))
    final_body = body if body != orig_body else None
    return reason, summary, spent[0], final_body, reliable_body


def _verdict_counts(records):
    counts = {}
    for record in records:
        counts[record['verdict']] = counts.get(record['verdict'], 0) + 1
    return counts


def _counts_line(counts):
    return ', '.join('%d %s' % (count, kind)
                     for kind, count in sorted(counts.items())) or 'no inputs'


def _record_strategy(table, strategy, payoff):
    if strategy.startswith('round-0'):
        return
    entry = table.setdefault(strategy, {'uses': 0, 'total': 0.0,
                                        'best': None})
    entry['uses'] += 1
    entry['total'] += payoff
    entry['best'] = payoff if entry['best'] is None \
        else max(entry['best'], payoff)


def _jsonable(abs_):
    return {'kind': abs_['kind'],
            'values': sorted(set(abs_['values']))[:64],
            'classes': sorted(str(c) for c in abs_['classes']),
            'offsets': {tag: list(span)
                        for tag, span in sorted(abs_['offsets'].items())},
            'wrote': {tag: ([min(o), max(o)] if o else None)
                      for tag, o in sorted(abs_['wrote'].items())},
            'ret_ikos': [[lo, hi] for lo, hi in abs_['ret_ikos']][:64],
            'cells_ikos': {tag: {str(off): list(span)
                                 for off, span in sorted(cells.items())}
                           for tag, cells in
                           sorted(abs_['cells_ikos'].items())},
            'cell_flags': sorted(abs_['cell_flags']),
            'ret_flag': abs_['ret_flag'],
            'witness': abs_.get('witness', [])[:64]}


def _game_state(plan, opt, history, strat_stats, real_a, spec_a, body,
                stopped, vetoes, reliable_body=None):
    return {'fn': plan.name, 'prompt_version': CO_PROMPT_VERSION,
            'domain': getattr(opt, 'domain', 'all'),
            'relation_class': difftest.relation_class(
                getattr(opt, 'domain', 'all')),
            'coefficients': {name: _co(opt, name)
                             for name in ('lambda', 'mu', 'alpha', 'beta',
                                         'epsilon', 'tau')},
            'policies': {'A': _co(opt, 'a_policy'),
                         'B': _co(opt, 'b_policy')},
            'violation_weight': VIOLATION_WEIGHT,
            'rounds': history, 'strategies': strat_stats,
            'abstractions': {'real': _jsonable(real_a),
                             'spec': _jsonable(spec_a)},
            'final_body': body, 'stopped': stopped, 'vetoes': vetoes,
            'reliable_body': reliable_body}


def _checkpoint(workdir, game, opt):
    try:
        path = os.path.join(workdir, 'game.json')
        tmp = path + '.tmp'
        with open(tmp, 'w') as stream:
            json.dump(game, stream, indent=1, sort_keys=True)
        os.replace(tmp, path)
        report_path = getattr(opt, 'co_report', None)
        if report_path:
            data = {}
            try:
                with open(report_path) as stream:
                    data = json.load(stream)
            except (OSError, ValueError):
                pass
            data[game['fn']] = game
            with open(report_path, 'w') as stream:
                json.dump(data, stream, indent=1)
    except OSError:
        pass  # diagnostics only, never worth failing the gate
