#include "Dataflow/DemandAPA/BpReader.h"
#include "Dataflow/DemandAPA/IfdsReader.h"
#include "Dataflow/DemandAPA/SPReader.h"

#include <gtest/gtest.h>

Algorithm<int> &demandAPAAlgorithmFromOtherTranslationUnit();
void *demandAPACacheFromOtherTranslationUnit();
bdd demandAPAIdentityFromOtherTranslationUnit();
int demandAPAProjectFromOtherTranslationUnit(int call, int summary);

TEST(DemandAPAHeadersTest, SharedStateAndProjectionLinkAcrossTranslationUnits) {
  EXPECT_EQ(&algSP, &demandAPAAlgorithmFromOtherTranslationUnit());
  EXPECT_EQ(static_cast<void *>(IfdsstarCache),
            demandAPACacheFromOtherTranslationUnit());

  algSP.vertexTypeG = {CALL_VERTEX, RETURN_SITE_VERTEX, START_VERTEX,
                       EXIT_VERTEX};
  algSP.callNodeInfo.assign(4, {-1, -1});
  algSP.callNodeInfo[0] = {1, 0};
  algSP.s = {2};
  algSP.e = {3};
  algSP.edgeListGInter = {{2, 3}, {(ll(3) << 32) | 1, 5}};
  EXPECT_EQ(demandAPAProjectFromOtherTranslationUnit(0, 7), 15);
  algSP = Algorithm<int>();
}

TEST(DemandAPAHeadersTest, BddAlgebraUsesOneIdentityAcrossTranslationUnits) {
  ASSERT_EQ(bdd_init(1000, 100), 0);
  ASSERT_EQ(bdd_setvarnum(4), 0);
  G = 1;
  L = 0;
  GL = 1;
  setPAone();
  {
    const bdd identity = demandAPAIdentityFromOtherTranslationUnit();
    const bdd negate = setNotEq(0);
    EXPECT_EQ(identity, setEq(0));
    EXPECT_EQ(PAdot(identity, negate), negate);
    EXPECT_EQ(PAdot(negate, identity), negate);
    EXPECT_EQ(PAplus(PAzero, negate), negate);
  }
  PAone = bddtrue;
  G = L = GL = 0;
  bdd_done();
}

TEST(DemandAPAHeadersTest,
     BasicDomainsAndRegexRemainUsableThroughReaderHeaders) {
  const vi relation{(1 << 16) | 1, 1, (1 << 16)};
  EXPECT_EQ(Ifdsdot(Ifdsone, relation), relation);
  EXPECT_EQ(Ifdsdot(relation, Ifdsone), relation);
  EXPECT_EQ(Ifdsplus(Ifdszero, relation), relation);
  EXPECT_EQ(SPdot(3, 5), 8);
  EXPECT_EQ(SPplus(3, 5), 3);

  std::ostringstream output;
  output << RegEx((ll(3) << 32) | 5);
  EXPECT_EQ(output.str(), "<3, 5>");
}

TEST(DemandAPAHeadersTest, NestedDebugOutputHasVisibleStreamOverloads) {
  std::ostringstream output;
  const std::map<int, std::vector<int>> values{{1, {2, 3}}};
  output << values;
  EXPECT_EQ(output.str(), "[{1 : [2, 3]}]");
}
