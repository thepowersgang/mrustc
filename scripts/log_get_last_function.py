import argparse
import sys

def is_function_header(line):
    if 'visit_function: ' in line \
        or 'evaluate_constant: ' in line \
        or 'Trans_Monomorphise_List: ' in line \
        or 'Trans_Codegen: FUNCTION CODE' in line \
        or 'Trans_Codegen- emit_' in line \
        or 'MIR_OptimiseInline: >> (' in line \
        :
        return True
    return None

def main():
    argp = argparse.ArgumentParser()
    argp.add_argument("-o", "--output", type=lambda v: open(v, 'w'), default=sys.stdout)
    argp.add_argument("logfile", type=open)
    argp.add_argument("fcn_name", type=str, nargs='*')
    args = argp.parse_args()

    fcn_lines = []
    found_fcn = False
    for line in args.logfile:
        if is_function_header(line) is not None:
            if found_fcn:
                break
            fcn_lines = []
            if len(args.fcn_name) > 0:
                for n in args.fcn_name:
                    if not n in line:
                        break
                    #print(n)
                else:
                    found_fcn = True
        fcn_lines.append(line.strip())

    for l in fcn_lines:
        args.output.write(l)
        args.output.write("\n")

if __name__ == "__main__":
    main()
