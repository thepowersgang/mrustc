import argparse
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

def main():
    arguments = get_arguments()

    templated = TEMPLATE_FILE.safe_substitute(
        MAJOR=arguments.major_version,
        MINOR=arguments.minor_version,
        PATCH=arguments.patch_version,
        GIT_ISDIRTY=1,
        GIT_FULLHASH='',
        GIT_SHORTHASH='',
        GIT_BRANCH='master',
        BUILDTIME='',
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
