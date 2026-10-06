// The registry of Java operator overrides TLC loads (by this class name)
// when the CommunityModules jar is on the class path. The jar ships its own,
// listing every module's overrides; this one comes first on the class path
// (see //third_party/tlaplus:tlc.bzl) and lists only the ones trace
// validation uses, because the jar's FiniteSetsExt override refers to a
// class (tlc2.value.impl.KSubsetValue) that TLC 1.7.4, the stable release
// pinned here, does not have, and loading it stops TLC. See README.md.
package tlc2.overrides;

public class TLCOverrides implements ITLCOverrides {
  @SuppressWarnings("rawtypes")
  @Override
  public Class[] get() {
    return new Class[] {IOUtils.class, Json.class, SequencesExt.class};
  }
}
