import argparse
import re
import log_get_last_function

RE_equate_lifetimes = re.compile(r".*equate_lifetimes: ('[^ ]+) := ('[^$]+)")
assert RE_equate_lifetimes.match("Expand HIR Lifetimes-       `anonymous-namespace'::ExprVisitor_Enumerate::equate_lifetimes: 'static := '#ivar14"), RE_equate_lifetimes.pattern
# Expand HIR Lifetimes-   HIR_Expand_LifetimeInfer_ExprInner: >> (=== Iter 0 ===)
# Expand HIR Lifetimes-    `anonymous-namespace'::LifetimeInferState::dump: '#ivar0 -- to=['#ivar69, '#ivar83], from=['#ivar13]
# Expand HIR Lifetimes-   HIR_Expand_LifetimeInfer_ExprInner: << (=== Iter 0 ===)
# Expand HIR Lifetimes-   HIR_Expand_LifetimeInfer_ExprInner: >> (COMPACT)
# Expand HIR Lifetimes-    `anonymous-namespace'::LifetimeInferState::dump: '#ivar0 = 'static to=['static, 'static]
# Expand HIR Lifetimes-   HIR_Expand_LifetimeInfer_ExprInner: << (COMPACT)

def main():
    argp = argparse.ArgumentParser()
    argp.add_argument("logfile", help="Single-function log file")
    argp.add_argument("--source", choices=['raw', 'initial', 'final'], default='raw')
    argp.add_argument("--only-ivar")
    args = argp.parse_args()
    
    class State(object):
        def __init__(self):
            self.links_raw = []
            self.links_initial = set() # from the initial ivar dump
            self.links_final = {}
            self.in_iter_0 = False
            self.in_compact = False
    fcn_name = ""
    state = State()
    for line in open(args.logfile):
        line = line.strip()
        if log_get_last_function.is_function_header(line):
            fcn_name = line
            state = State()
            continue
        
        if ' >> (=== Iter 0 ===)' in line:
            state.in_iter_0 = True
            continue
        if ' << (=== Iter 0 ===)' in line:
            state.in_iter_0 = False
            continue
        if ' >> (COMPACT)' in line:
            state.in_compact = True
            continue
        if ' << (COMPACT)' in line:
            state.in_compact = False
            continue
        
        m = RE_equate_lifetimes.match(line)
        if m is not None:
            state.links_raw.append( (m.group(2), m.group(1), "") )
            continue
        
        if state.in_iter_0 and 'dump: ' in line and ' -- to=' in line:
            _,line = line.split('dump: ')
            if ' = ' in line:
                continue
            varname,extra = line.split(' -- ')
            
            extra = extra[4:-1]
            to,fr = extra.split('], from=[')
            
            if to != "":
                for v in to.split(', '):
                    state.links_initial.add((varname, v, ""))
            if fr != "":
                for v in fr.split(', '):
                    state.links_initial.add((v, varname, ""))
            continue
        
        if state.in_compact and "dump: '" in line and " = '" in line:
            _,line = line.split('dump: ')
            if not ' = ' in line:
                continue
            varname,extra = line.split(' = ')
            
            fr,to = extra.split(' to=[')
            to = to[:-1]
            
            if to != "":
                for v in to.split(', '):
                    state.links_final.setdefault((fr, v, ), set()).add(varname)
            continue
    state.links_final = set( (a,b,make_list(vals)) for (a,b),vals in state.links_final.items() )
    
    links = {
        'raw': state.links_raw,
        'initial': state.links_initial,
        'final': state.links_final,
        }[args.source]
    if args.only_ivar is not None:
        links = get_subgraph_for_ivar(links, "'#ivar"+args.only_ivar)
    
    if True:
        print()
        print("digraph borrowck {")
        for a,b,label in links:
            print("\"{a}\" -> \"{b}\" {{ label = \"{label}\" }};".format(a=a,b=b, label=label))
        print("}")

    if False:
        import networkx as nx
        import matplotlib.pyplot as plt
        g = nx.DiGraph()
        for a,b,label in links:
            if a.startswith("'#ivar"):
                a = a[6:]
            else:
                a = a + '-T'
            if b.startswith("'#ivar"):
                b = b[6:]
            else:
                b = b + '-B'
            g.add_edge(a, b)
        pos = None
        #pos = nx.planar_layout(g)
        pos = nx.kamada_kawai_layout(g) # Gives a decent layout without needing graphviz/dot
        #pos = nx.nx_agraph.graphviz_layout(g)
        #pos = nx.nx_pydot.graphviz_layout(g)
        nx.draw(g, with_labels=True, arrows=True, pos=pos)
        plt.show()
    elif False:
        import graphviz
        g = graphviz.Digraph('borrowck')
        for a,b,label in links:
            g.edge(a, b, label=label)
        g.view()
    else:
        print()
        print("digraph borrowck {")
        for a,b,label in links:
            print("\"{a}\" -> \"{b}\"; # ".format(a=a,b=b), label)
        print("}")


def make_list(vals):
    vals = list(vals)
    rv = ""
    for i in range(0, len(vals), 5):
        rv += ", ".join(vals[i:][:5])
        rv += "\n"
    return rv

def get_subgraph_for_ivar(links, name):
    assert name.startswith("'#ivar")
    return get_tree(links, name, 0) + get_tree(links, name, 1)

def get_tree(links, root_name, dir=0):
    rv = []
    stack = [root_name]
    visited = set()
    while len(stack) > 0:
        n = stack[-1]; del stack[-1]
        if n in visited:
            continue
        visited.add(n)
        if not n.startswith("'#ivar"):
            continue
        for l in links:
            if l[dir] == n:
                rv.append(l)
                stack.append( l[1-dir] )
    return rv

if __name__ == "__main__":
    main()
