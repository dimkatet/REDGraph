"""An RT library that carries a RAYGEN VARIANT next to the engine's own raygen (docs/render-graph.md "RT per node").

The engine's state-object creator adds the DXIL library with no export list (= export all), and GpuApi::DispatchRays
asks the state object for the raygen's identifier BY NAME on every dispatch. So a library holding the vanilla raygen
AND a renamed copy of it bakes into the SAME state object; the host's rt_variant (M12) hands the copy's identifier to
the dispatches of one node. Miss / hit tables, the root signature and the other exports stay the engine's.

This tool clones one raygen function of a clean library under a new name, registers the clone as a raygeneration entry
point (same props as the original) and applies an edit to the CLONE only:
  lit   - every textureStore writes x = 65504 (the RT sun-shadow raygens' "lit" value) in value0 / value2, the
          view-Z channels untouched: the self-test variant of 318cee44 (RT sun shadows vanish where it runs).
  none  - an exact copy (the mechanism alone: nothing may change on screen).
With --vanilla CLEAN_LL the input is instead a library some edit tool already changed (e.g. the grass contact shadow):
its edited raygen becomes the variant and the engine's raygen from CLEAN_LL goes back under the original name - so the
vanilla dispatches run the engine's exact code (an edit left in the engine's raygen would cost even while off).

With --only (or --strip on a library already built) the output keeps ONLY the variants (every shader named --prefix*,
your mod's tag): the host ADDS such a library next to the engine's own in the state object and exports just the variants,
so the engine's code in our blob would be dead weight. Other shader definitions and their entry-point rows go; types,
globals, resources and declarations stay (unused ones are harmless in a library).

Usage: python tools/rt_raygen_variant.py <clean.ll> <out.ll> --clone rgs_shadow_main --as mymod_rgs_shadow_lit \
           [--edit lit] [--only --prefix mymod_]
       python tools/rt_raygen_variant.py <edited.ll> <out.ll> --clone rgs_shadow_main --as mymod_rgs_shadow \
           --vanilla <clean.ll> [--only --prefix mymod_]
       python tools/rt_raygen_variant.py <built.ll> <out.ll> --strip --prefix mymod_
"""
import argparse
import re
import sys

LIT = '6.550400e+04'


def mangled(name):
    return '"\\01?' + name + '@@YAXXZ"'


def lit_store(ln):
    """One IR line: a textureStore.f32 rewritten to write LIT in value0 / value2 (other lines unchanged)."""
    if '@dx.op.textureStore.f32(' not in ln:
        return ln
    m = re.match(r'(\s*call void @dx\.op\.textureStore\.f32\()(.*?)(\)\s*(;.*)?)$', ln)
    if not m:
        sys.exit(f'unparsed store: {ln}')
    args = [s.strip() for s in m.group(2).split(',')]
    # (i32 67, handle, c0, c1, c2, v0, v1, v2, v3, mask)
    if len(args) != 10:
        sys.exit(f'store with {len(args)} args: {ln}')
    args[5] = f'float {LIT}'
    args[7] = f'float {LIT}'
    return m.group(1) + ', '.join(args) + m.group(3)


def strip(lines, prefix):
    """Keep only the `<prefix>*` shader definitions and their entry-point rows (the library row `!{null, ...}` stays)."""
    sym = re.compile(r'^define [^@]*@("\\01\?(\w+)@@[^"]*")\(')
    out, i, dropped, kept = [], 0, [], []
    while i < len(lines):
        m = sym.match(lines[i])
        if m:
            end = i
            while lines[end] != '}':
                end += 1
            if m.group(2).startswith(prefix):
                kept.append(m.group(2))
                out += lines[i:end + 1]
            else:
                dropped.append(m.group(1))
                while out and out[-1] == '':  # one blank line where the definition was
                    out.pop()
                out.append('')
                if end + 1 < len(lines) and lines[end + 1] == '':
                    end += 1
            i = end + 1
            continue
        out.append(lines[i])
        i += 1
    if not kept:
        sys.exit(f'strip: no {prefix}* shader in the library')
    rows = {}
    for j, l in enumerate(out):
        m = re.match(r'^!(\d+) = !\{[^@!]*@("[^"]+"), ', l)
        if m and m.group(2) in dropped:
            rows[m.group(1)] = j
    if len(rows) != len(dropped):
        sys.exit(f'strip: {len(dropped)} shader(s) dropped but {len(rows)} entry-point row(s) found')
    ep = next(j for j, l in enumerate(out) if l.startswith('!dx.entryPoints = !{'))
    ids = [x.strip() for x in out[ep][len('!dx.entryPoints = !{'):-1].split(',')]
    out[ep] = '!dx.entryPoints = !{' + ', '.join(x for x in ids if x[1:] not in rows) + '}'
    for j in sorted(rows.values(), reverse=True):
        del out[j]
    return prune(out), kept, dropped


def prune(lines):
    """The validator refuses unused declarations: drop every `declare` and resource global no function body names,
    their rows from the dx.resources lists, and renumber the resource IDs per class (a library reaches a resource
    through its global - createHandleForLib - never through the ID)."""
    bodies, inside = [], False
    for l in lines:
        if l.startswith('define '):
            inside = True
        if inside:
            bodies.append(l)
        if l == '}':
            inside = False
    used = '\n'.join(bodies)

    def named(name):
        return re.search(re.escape('@' + name) + r'(?![\w.$])', used) is not None

    glob_re = re.compile(r'^@("[^"]+"|[\w.$]+) = ')
    decl_re = re.compile(r'^declare [^@]*@("[^"]+"|[\w.$]+)\(')
    gone = set()
    out = []
    for l in lines:
        m = glob_re.match(l) or decl_re.match(l)
        if m and not m.group(1).startswith('llvm.') and not named(m.group(1)):
            gone.add('@' + m.group(1))
            if out and out[-1].startswith('; Function Attrs:'):
                out.pop()
            continue
        out.append(l)
    if not gone:
        return out
    # Resource rows: !N = !{i32 ID, %type* @global, !"name", ...}; class lists: !dx.resources = !{!srv, !uav, !cbv, !smp}.
    md = {m.group(1): i for i, m in ((i, re.match(r'^!(\d+) = ', l)) for i, l in enumerate(out)) if m}
    res_top = next(l for l in out if l.startswith('!dx.resources = !{'))
    top_id = re.match(r'^!dx\.resources = !\{!(\d+)\}$', res_top).group(1)
    top_i = md[top_id]
    classes = re.findall(r'!(\d+)|null', out[top_i][out[top_i].index('!{'):])
    new_classes, dead = [], []
    for c in classes:
        if not c:
            new_classes.append('null')
            continue
        li = md[c]
        members = re.findall(r'!(\d+)', out[li][out[li].index('!{'):])
        keep = []
        for r in members:
            row = out[md[r]]
            g = re.search(r'\* (@("[^"]+"|[\w.$]+)), ', row)
            if g and g.group(1) in gone:
                dead.append(md[r])  # the row names the dropped global: it goes too
                continue
            keep.append(r)
        for new_id, r in enumerate(keep):  # IDs 0..n-1 in the class, in order
            out[md[r]] = re.sub(r'^(!\d+ = !\{i32 )\d+,', lambda m: f'{m.group(1)}{new_id},', out[md[r]])
        if keep:
            out[li] = f'!{c} = !{{' + ', '.join('!' + r for r in keep) + '}'
            new_classes.append('!' + c)
        else:
            new_classes.append('null')
    out[top_i] = f'!{top_id} = !{{' + ', '.join(new_classes) + '}'
    for i in sorted(dead, reverse=True):
        del out[i]
    left = [g for g in gone if re.search(re.escape(g) + r'(?![\w.$])', '\n'.join(out))]
    if left:
        sys.exit(f'prune: dropped {left[:3]} but they are still named')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--clone', help='the raygen to copy (unmangled, e.g. rgs_shadow_main)')
    ap.add_argument('--as', dest='new', help='the variant\'s export name (unmangled)')
    ap.add_argument('--only', action='store_true', help='output only the variants (the --prefix shaders)')
    ap.add_argument('--strip', action='store_true', help='src is a built library: keep only its --prefix shaders')
    ap.add_argument('--prefix',
                    help='--only / --strip keep the shaders whose name starts with this (your mod\'s tag, e.g. mymod_)')
    ap.add_argument('--edit', choices=['lit', 'none'], default='none')
    ap.add_argument('--vanilla', metavar='CLEAN_LL',
                    help='src is an EDITED library: its raygen becomes the variant and the raygen of CLEAN_LL (the '
                         'engine\'s own, the edit\'s input) is put back under the original name')
    a = ap.parse_args()
    if (a.only or a.strip) and not a.prefix:
        sys.exit('--only / --strip need --prefix (your mod\'s tag, e.g. mymod_)')
    if a.strip:
        out, kept, dropped = strip(open(a.src, encoding='utf-8').read().split('\n'), a.prefix)
        open(a.out, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
        print(f'{a.out}: kept {", ".join(kept)}; dropped {len(dropped)} shader(s)')
        return
    if not a.clone or not a.new:
        sys.exit('--clone and --as are required (or --strip)')
    if a.vanilla and a.edit != 'none':
        sys.exit('--vanilla takes the edit from src; --edit must be none')

    text = open(a.src, encoding='utf-8').read()
    lines = text.split('\n')
    old_sym, new_sym = mangled(a.clone), mangled(a.new)
    if new_sym in text:
        sys.exit(f'{a.new} already in {a.src}')

    head = f'define void @{old_sym}() #0 {{'
    try:
        start = lines.index(head)
    except ValueError:
        sys.exit(f'no "{head}" in {a.src} (a raygen is void(), attributes #0)')
    end = start
    while lines[end] != '}':
        end += 1
    body = lines[start:end + 1]

    stores = 0
    clone = [f'define void @{new_sym}() #0 {{']
    for ln in body[1:]:
        if a.edit == 'lit' and '@dx.op.textureStore.f32(' in ln:
            ln = lit_store(ln)
            stores += 1
        clone.append(ln)
    if a.edit == 'lit' and stores == 0:
        sys.exit('edit lit: no textureStore in the raygen')

    # The entry point: the original's metadata row, with the clone's symbol and name.
    ent_re = re.compile(r'^!(\d+) = !\{void \(\)\* @' + re.escape(old_sym) + r', !' + re.escape(old_sym) +
                        r', null, null, !(\d+)\}$')
    ent = [m for m in (ent_re.match(l) for l in lines) if m]
    if len(ent) != 1:
        sys.exit(f'entry-point row of {a.clone}: {len(ent)} found')
    props = ent[0].group(2)
    ids = [int(m.group(1)) for m in (re.match(r'^!(\d+) = ', l) for l in lines) if m]
    new_id = max(ids) + 1
    ep_i = next(i for i, l in enumerate(lines) if l.startswith('!dx.entryPoints = !{'))
    lines[ep_i] = lines[ep_i][:-1] + f', !{new_id}}}'

    orig = body
    if a.vanilla:
        # The engine's raygen from the clean library. The edits only ADD module-level lines (types, globals,
        # declarations, metadata nodes at new ids), so everything the clean body names must exist in src unchanged.
        clines = open(a.vanilla, encoding='utf-8').read().split('\n')
        try:
            cs = clines.index(head)
        except ValueError:
            sys.exit(f'no "{head}" in {a.vanilla}')
        ce = cs
        while clines[ce] != '}':
            ce += 1
        orig = clines[cs:ce + 1]
        defined = set(lines[:start] + lines[end + 1:])
        used_md = set(re.findall(r'!(\d+)\b', '\n'.join(orig)))

        def needed(l):  # types / globals / declarations / attributes, and the metadata nodes the body references;
            if not l or l.startswith(';') or l.startswith('!dx.') or l.startswith('!llvm.'):  # the resource lists
                return False                                       # the edit appended to may differ
            m = re.match(r'^!(\d+) = ', l)
            return not m or m.group(1) in used_md
        bad = [l for l in clines[:cs] + clines[ce + 1:] if needed(l) and l not in defined]
        if bad:
            sys.exit('the clean raygen needs module lines src lacks or changed:\n  ' + '\n  '.join(bad[:10]))
        if orig == body:
            sys.exit(f'{a.clone} is identical in src and {a.vanilla} - src carries no edit')

    out = lines[:start] + orig + [''] + clone + lines[end + 1:]
    last_md = max(i for i, l in enumerate(out) if re.match(r'^!\d+ = ', l))
    out.insert(last_md + 1, f'!{new_id} = !{{void ()* @{new_sym}, !{new_sym}, null, null, !{props}}}')
    if a.only:
        out, _, _ = strip(out, a.prefix)
    open(a.out, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print(f'{a.out}: {a.clone} cloned as {a.new} ({len(body)} lines, edit {a.edit}: {stores} store(s) changed), '
          f'entry !{new_id} props !{props}')


if __name__ == '__main__':
    main()
