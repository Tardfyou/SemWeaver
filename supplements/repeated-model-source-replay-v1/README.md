# Repeated-model and controlled-source supplement

This versioned supplement accompanies the final SemWeaver paper. It closes two
small follow-ups: three native-editor decodes for each of three models on the
same twelve CSA subjects, and a source-form replay on three original Linux
subjects. It does not replace the frozen 39-subject comparison or the earlier
target-validation supplement.

## Offline review

From the anonymous repository root, first verify and restore the small-file
transport into a new directory:

```sh
python3 supplements/repeated-model-source-replay-v1/restore_review.py verify supplements/repeated-model-source-replay-v1
python3 supplements/repeated-model-source-replay-v1/restore_review.py restore supplements/repeated-model-source-replay-v1 --output ../repeat-replay-exact
python3 ../repeat-replay-exact/verify_recorded.py ../repeat-replay-exact
```

The last command uses Python 3.10+ standard library only. It checks the public
byte manifest, recomputes the three-run model scores from recorded TP/FP/FN,
and checks the source-replay matrix and selected-case transitions. It does not
call a model, compile Linux, rerun CSA or claim bit-identical future decodes.
The original E4 first runs are in the preexisting main transport; this package
adds the 72 new trial folders and their received replies, checker versions,
validation results and run manifests. The source replay includes frozen source
variants and recorded scan outputs. See [METHODS.md](METHODS.md) for counting
and provenance boundaries.

All release documentation is in English. Original model replies and tool output
remain in their received language. Private workstation paths and gateway names
are consistently replaced in public text; the manifest distinguishes hashes of
the public bytes from hashes of frozen source originals. No credentials are
included. No project-wide license is granted; supplied for scholarly review.
