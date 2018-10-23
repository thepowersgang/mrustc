import argparse
from datetime import datetime
import string

def get_arguments():
    parser = argparse.ArgumentParser(
        description= 'Create a version.cpp file - this is run by meson on every build',
        allow_abbrev=False,
    )

    parser.add_argument(
        '--output-file',
        action='store',
        metavar='PATH',
        required=True,
        help='Where to put the generated version file',
    )

    parser.add_argument(
        '--major-version',
        action='store',
        metavar='X',
        required=True,
        type=int,
        help='Major version number',
    )

    parser.add_argument(
        '--minor-version',
        action='store',
        metavar='Y',
        required=True,
        type=int,
        help='Minor version number',
    )

    parser.add_argument(
        '--patch-version',
        action='store',
        metavar='Z',
        required=True,
        type=int,
        help='Patch version number',
    )

    return parser.parse_args()

def git_info():
    from subprocess import run
    is_dirty = 1
    fullhash = ''
    shorthash = ''
    branch = ''

    try:
        is_dirty_output = run(
            ['git', 'status', '--porcelain'],
            capture_output=True)

        if not is_dirty_output.stdout:
            is_dirty = 0

        fullhash_output = run(
            ['git', 'rev-parse', 'HEAD'],
            capture_output=True
        )
        fullhash = fullhash_output.stdout.decode().strip()

        shorthash_output = run(
            ['git', 'rev-parse', '--short', 'HEAD'],
            capture_output=True,
        )
        shorthash = shorthash_output.stdout.decode().strip()

        branch_output = run(
            ['git', 'rev-parse', '--abbrev-ref', 'HEAD'],
            capture_output=True,
        )
        branch = branch_output.stdout.decode().strip()
    except:
        pass

    return {
        'GIT_ISDIRTY': is_dirty,
        'GIT_FULLHASH': fullhash,
        'GIT_SHORTHASH': shorthash,
        'GIT_BRANCH': branch,
    }

def main():
    arguments = get_arguments()

    buildtime = datetime.utcnow().isoformat()

    templated = TEMPLATE_FILE.safe_substitute(
        git_info(),
        MAJOR=arguments.major_version,
        MINOR=arguments.minor_version,
        PATCH=arguments.patch_version,
        BUILDTIME=buildtime,
    )

    with open(arguments.output_file, 'w') as f:
        f.write(templated)

TEMPLATE_FILE = string.Template('''
#include <version.hpp>
#include <sstream>

const unsigned int giVersion_Major = $MAJOR;
const unsigned int giVersion_Minor = $MINOR;
const unsigned int giVersion_Patch = $PATCH;
const bool gbVersion_GitDirty = $GIT_ISDIRTY;
const char gsVersion_GitHash[] = "$GIT_FULLHASH";
const char gsVersion_GitShortHash[] = "$GIT_SHORTHASH";
const char gsVersion_GitBranch[] = "$GIT_BRANCH";
const char gsVersion_BuildTime[] = "$BUILDTIME";


::std::string Version_GetString()
{
    ::std::stringstream ss;
    ss << 'v' << giVersion_Major << '.' << giVersion_Minor << '.' << giVersion_Patch << ' ' << gsVersion_GitBranch << ':' << gsVersion_GitShortHash;
    return ss.str();
}
''')

if __name__ == '__main__':
    main()
