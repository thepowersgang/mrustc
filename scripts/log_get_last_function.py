import argparse
import sys

def main():
    argp = argparse.ArgumentParser()
    argp.add_argument("-o", "--output", type=lambda v: open(v, 'w'), default=sys.stdout)
    argp.add_argument("logfile", type=open)
    argp.add_argument("fcn_name", type=str, nargs='?')
    args = argp.parse_args()

    fcn_lines = []
    found_fcn = False
    for line in args.logfile:
        if 'visit_function: ' in line \
            or 'evaluate_constant: ' in line \
            or 'Trans_Monomorphise_List: ' in line \
            or 'Trans_Codegen: FUNCTION CODE' in line \
            or 'Trans_Codegen- emit_' in line \
            or 'MIR_OptimiseInline: >> (' in line \
            :
            if found_fcn:
                break
            fcn_lines = []
            if args.fcn_name is not None and args.fcn_name in line:
                found_fcn = True
        fcn_lines.append(line.strip())

    for l in fcn_lines:
        args.output.write(l)
        args.output.write("\n")

main()
