% MIR Match Generation Algorithms and Notes

Alternative DecisionTree generator
=================================

Take the full list of rules and iteratively partition and simplify/propagate
- Requires that all rules have the same format (which they do)


For each index in the rule (all rules must be the same length)
- Sort the rules (with `_` sorting to the end)
- If all rules are `_`, skip and continue
- Group into sub-lists based on the condition
- Add copies of the default to all sub-lists
 - NOTE: Default is maintained only if the ruleset is not exhaustive at this level


- TODO: The above approach beaks with value ranges (which can overlap)
 - Requires special handling to resolve the overlaps
 - Ranges sort after?



Alternative Generator 2
=======================

Maintains match ordering

1. Calculate branch rulesets (as existing)
1. While rules to process:
 1. Group based on shared values.
 1. Generate dispatch arm for each group
 1. Recurse into group, passing local _ as fallback (or parent _ if none)

```

for 

```


