# FastCluster

Vendored copy of Daniel Müllner's
[fastcluster](http://danifold.net/fastcluster.html), used by
`Utils/ADT/HierarchicalClusterer` for object-ID clustering. The code is
licensed under the BSD license in `LICENSE.txt`.

Lotus uses PascalCase file names for sources and headers, so the upstream files
were renamed without changing their contents (apart from include paths):

| Upstream | Lotus |
| --- | --- |
| `fastcluster.h` | `FastCluster.h` |
| `fastcluster.cpp` | `FastCluster.cpp` |
| `fastcluster_dm.cpp.inc` | `FastClusterLinkage.inc` |
| `fastcluster_R_dm.cpp.inc` | `FastClusterDendrogram.inc` |

Keep the upstream copyright headers and `LICENSE.txt` when updating the
vendored copy.
