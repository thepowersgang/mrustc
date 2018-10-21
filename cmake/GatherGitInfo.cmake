include(GetGitRevisionDescription)

#! gather_git_info : Populates variables with the current commit hash, branch, ...
#
# Defines the following variables:
#
#  GIT_IS_DIRTY - Truthful if there are uncommited changes
#  GIT_HEAD_REF - Head ref., eg. "refs/head/master"
#  GIT_HEAD_HASH - HEADs commit hash (long)
#  GIT_HEAD_HASH_SHORT - HEADs commit hash (short - 7 chars)
#  GIT_BRANCH - Current branch
#
function(gather_git_info)
  git_local_changes(GIT_IS_DIRTY)
  get_git_head_revision(GIT_HEAD_REF GIT_HEAD_HASH "--pretty=%h") # Get short hash
  string(SUBSTRING ${GIT_HEAD_HASH} 0 7 GIT_HEAD_HASH_SHORT)
  string(REPLACE "/" ";" GIT_HEAD_REF_AS_LIST ${GIT_HEAD_REF}) # Split eg. refs/head/master
  list(REVERSE GIT_HEAD_REF_AS_LIST)
  list(GET GIT_HEAD_REF_AS_LIST 0 GIT_BRANCH)

  set(GIT_IS_DIRTY ${GIT_IS_DIRTY} PARENT_SCOPE)
  set(GIT_HEAD_REF ${GIT_HEAD_REF} PARENT_SCOPE)
  set(GIT_HEAD_HASH ${GIT_HEAD_HASH} PARENT_SCOPE)
  set(GIT_HEAD_HASH_SHORT ${GIT_HEAD_HASH_SHORT} PARENT_SCOPE)
  set(GIT_BRANCH ${GIT_BRANCH} PARENT_SCOPE)
endfunction()
