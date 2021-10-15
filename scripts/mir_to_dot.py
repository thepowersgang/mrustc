import re
import argparse

class Link(object):
    def __init__(self, src, dst, label):
        self._src = 'bb%i' % (src,)
        self._dst = dst if isinstance(dst, str) else 'bb%i' % (dst,)
        self._label = label

def main():

    #cratename,pat = 'rustc_lint','fn .*expr_refers_to_this_method.*'
    cratename,pat = 'std','fn resize.*HashMap'
    #cratename,pat = 'rustc', 'fn tables.*::"rustc"::ty::context::TyCtxt'

    argp = argparse.ArgumentParser()
    argp_src = argp.add_mutually_exclusive_group(required=True)
    argp_src.add_argument("--file", type=str)
    argp_src.add_argument("--crate", type=str)
    argp.add_argument("--fn-name", type=str, required=True)
    args = argp.parse_args()

    pat = 'fn '+args.fn_name
    infile = args.file or ('output/'+args.crate+'.hir_3_mir.rs')

    fp = open(infile)
    start_pat = re.compile(pat)
    def_line = None
    for line in fp:
        line = line.strip()
        if start_pat.match(line) != None:
            print("# ",line)
            def_line = line
            break

    if def_line is None:
        return

    for line in fp:
        if line.strip() == "bb0: {":
            break

    bbs = []
    cur_bb_lines = []
    level = 2
    for line in fp:
        line = line.strip()
        if line == "}":
            level -= 1
            if level == 0:
                break
            else:
                bbs.append( cur_bb_lines )
                cur_bb_lines = []
                continue

        if "bb" in line and ": {" in line:
            level += 1
            continue

        outstr = ""
        comment_level = 0
        i = 0
        while i < len(line):
            if comment_level > 0:
                if line[i:i+2] == '*/':
                    comment_level -= 1
                    i += 2
                    continue
            if line[i:i+2] == '/*':
                comment_level += 1
                i += 2
                continue 
            if comment_level == 0:
                outstr += line[i]
            i += 1
        print("#",len(bbs),outstr)

        cur_bb_lines.append(outstr)

    goto_regex = re.compile('goto bb(\d+);$')
    call_regex = re.compile('.*goto bb(\d+) else bb(\d+)$')
    if_regex = re.compile('.*goto bb(\d+); } else { goto bb(\d+); }$')
    switch_regex = re.compile('(\d+) => bb(\d+),')

    links = []
    for idx,bb in enumerate(bbs):
        if bb[-1] == 'return;':
            links.append( Link(idx, 'return', "return") )
            continue
        if bb[-1] == 'diverge;':
            #links.append( Link(idx, 'panic', "diverge") )
            continue
        m = goto_regex.match(bb[-1])
        if m != None:
            links.append( Link(idx, int(m.group(1)), "") )
            continue
        m = call_regex.match(bb[-1])
        if m != None:
            links.append( Link(idx, int(m.group(1)), "ret") )
            #links.append( Link(idx, int(m.group(2)), "panic") )
            continue
        m = if_regex.match(bb[-1])
        if m != None:
            links.append( Link(idx, int(m.group(1)), "true") )
            links.append( Link(idx, int(m.group(2)), "false") )
            continue


        for m in switch_regex.finditer(bb[-1]):
            links.append( Link(idx, int(m.group(2)), "var%s" % (m.group(1),) ) )



    print("digraph {")
    print("node [shape=box, labeljust=l; fontname=\"mono\"];")
    for l in links:
        print('"{}" -> "{}" [label="{}"];'.format(l._src, l._dst, l._label))

    print("")
    for idx,bb in enumerate(bbs):
        print('"bb{0}" [label="BB{0}:'.format(idx), end="")
        for stmt in bb:
            print('\\l',stmt.replace('"', '\\"'), end="")
        print('"];')
    print("}")


main()

