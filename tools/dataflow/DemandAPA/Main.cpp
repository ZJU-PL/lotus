#include "Dataflow/DemandAPA/Algorithm.h"
#include "Dataflow/DemandAPA/BpReader.h"
#include "Dataflow/DemandAPA/IfdsReader.h"
#include "Dataflow/DemandAPA/Project.h"
#include "Dataflow/DemandAPA/SPReader.h"
#include "Dataflow/DemandAPA/Support.h"

#include <dirent.h>

int main(int argc, char *argv[]) {
#ifdef LOCAL
  auto stTime = omp_get_wtime();
#endif
  ios::sync_with_stdio(false);
  cout << fixed;
  cout.precision(10);
  cin.tie(0);

  if (argc != 5) {
    cerr << "usage: lotus-demand-apa TIMEOUT COUNT RESET dataset-root\n";
    return 2;
  }

  const int timeout = stoi(argv[1]);
  int programCnt = stoi(argv[2]);
  int reset = stoi(argv[3]);

  db(timeout);

  srand(1);

  string path = argv[4];
  if (path.empty()) {
    cerr << "dataset-root must not be empty\n";
    return 2;
  }
  if (path.back() != '/')
    path += '/';

  set<pair<string, string>> exclude;

  ifstream fIn(path + "results.csv");
  bool fileExists = fIn.good();
  fIn.close();

  if (reset || !fileExists) {
    ofstream fOut(path + "results.csv");
    if (!fOut) {
      cerr << "cannot write " << path << "results.csv\n";
      return 1;
    }
    fOut << "analysis, program, nG, mG, nCallGraph, mCallGraph, tw, td, Proc, "
            "functionSummaryTime, totQueryTime, "
         << "OAR, totNaiveQueryTime, BAR, ratio, ratioNaive, qNaiveAnswered, "
            "numQueries, performanceRatio\n";
    fOut.close();
  } else {
    ifstream fIn(path + "results.csv");
    string s;
    while (getline(fIn, s)) {
      if (s.empty())
        continue;
      // cout << s << endl;
      string analysis = "", program = "";
      int j = 0;
      for (int i = 0; i < sz(s); ++i) {
        if (s[i] == ',') {
          if (j == 0) {
            analysis = s.substr(0, i - j);
            j = i + 1;
          } else {
            program = s.substr(j, i - j);
            break;
          }
        }
      }
      exclude.insert(mp(program, analysis));
    }
    // cout << exclude << endl;
  }

  double t;

  V<string> analyses;

  analyses.pb("reachability");
  analyses.pb("null_ptr");
  analyses.pb("uninit_var");
  analyses.pb("BP");

  int MAX_GL = 95; // the maximum GL over the boolean programs we run

  for (auto &analysis : analyses) {

    V<string> programs;
    {
      DIR *dir;
      struct dirent *ent;
      if ((dir = opendir(
               (string(path + "prep_output/" + analysis + "/")).c_str())) !=
          NULL) {
        while ((ent = readdir(dir)) != NULL) {
          string fileName = ent->d_name;
          if (fileName.size() > 4 &&
              fileName.substr(fileName.size() - 4) == ".txt")
            programs.pb(fileName);
        }
        closedir(dir);
      }
    }
    if (programs.empty())
      continue;
    if (analysis == "BP") {
      int nodes = 100000000;
      int cache = 10000000;
      if (const char *value = getenv("LOTUS_DEMAND_APA_BDD_NODES"))
        nodes = stoi(value);
      if (const char *value = getenv("LOTUS_DEMAND_APA_BDD_CACHE"))
        cache = stoi(value);
      if (bdd_init(nodes, cache) != 0 || bdd_setvarnum(4 * MAX_GL) != 0) {
        cerr << "failed to initialize BuDDy\n";
        return 1;
      }
    }

    db(analysis);
    db(programs);

    if (programCnt)
      while (sz(programs) > programCnt)
        programs.pop_back();

    int f = 0;
    for (auto program : programs) {

      if (present(exclude, mp(program.substr(0, sz(program) - 4), analysis)))
        continue;

      cout << "----------------------------------------------------------------"
              "--------------------\n";
      cout << ++f << ": " << program << ", " << analysis << endl;

      RowInExcelSheet row(path + "results.csv");

      row.analysis = analysis;
      row.program = program.substr(0, sz(program) - 4);

      string pathToFile = path + "prep_output/" + analysis + "/" + program;
      string TWDpath =
          path + "treewidth_solver_output/" + analysis + "/" + program;
      string TDDpath =
          path + "treedepth_solver_output/" + analysis + "/" + program;

      if (analysis != "BP") {
        IfdsReader reader = IfdsReader(pathToFile, TWDpath, TDDpath);

        ApaInstance<vi> IfdsInstance = reader.getInstance();

        algIfds = Algorithm<vi>();
        algIfds.work(&IfdsInstance, row, 8, timeout);
      } else {

        BpReader reader = BpReader(pathToFile, TWDpath, TDDpath);

        ApaInstance<bdd> BPInstance = reader.getInstance();
        //				cout << "Instance generated
        // successfully!" << endl;
        algBp = Algorithm<bdd>();

        algBp.work(&BPInstance, row, 1, timeout);
      }

      cout << "----------------------------------------------------------------"
              "--------------------\n";
    }

    if (analysis == "BP")
      bdd_done();
  }

#ifdef LOCAL
  cout << "\n\n\nExecution time: " << (omp_get_wtime() - stTime) << " s"
       << endl;
#endif
  return 0;
}
