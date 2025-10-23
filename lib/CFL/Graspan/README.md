# Graspan C++

Welcome to the home repository of Graspan C++.

Graspan is a **disk-based parallel graph system** that uses an edge-pair centric computation model to compute dynamic transitive closures on large program graphs. Graspan has been implemented in two languages: Java and C++. This repository provides the C++ implementation of Graspan.

This Readme (under revision) provides a how-to-use guide for Graspan C++. To see how to use the **Java version** of Graspan, look [here](https://github.com/Graspan/graspan-java).

See [here](https://github.com/Graspan/graspan-java/blob/master/AboutGraspan.md) to understand **how Graspan works** and how it performs in comparison to other systems (under revision).

For a detailed description of our system, please see the **preliminary version of our paper** [here](http://www.ics.uci.edu/~guoqingx/papers/wang-asplos17.pdf), which has been accepted in ASPLOS 2017. In addition, a tutorial of Graspan has been presented in ASPLOS 2017. If you are interested, visit our **ASPLOS 2017 tutorial** [page](http://web.cs.ucla.edu/~harryxu/asplos-tutorial/main.html). 

## Getting Started

Graspan C++ is simple to use, with a very straight-forward compilation procedure. 

### Required Libraries
Ensure that you have a recent version of the Boost library installed in your system. You can obtain the library from [here](http://www.boost.org/users/history/version_1_62_0.html). 

### Compiling GraspanCPP
First, download the entire **graspan-cpp** source code into your machine. 
Next, edit the **src/makefile** to set the paths to the Boost library include files and lib files in your machine. Do the same for the **src/run** file. Finally, run the makefile in the **src** folder using **make**. GraspanCPP should now be compiled and ready to run. 

### Running GraspanCPP

Graspan needs two input files: (1) a graph on which Graspan can perform computations and (2) a grammar file which describes how the computations (edge additions) are to be performed.

You may copy any graph and grammar file from our sample datasets [here](https://drive.google.com/drive/folders/1M4WxwykUd-jX8jBA50pSNf2R-1IJ49PJ?usp=drive_link) inside the **graspan-cpp/src** folder in your machine. 

*Note that Graspan supports graph files in edge-list format as shown below,*

```
[EDGE SOURCE] [EDGE DESTINATION] [EDGE VALUE]
```

*The grammar file consists of production rules in CNF form, where a line like the following,* 
```
A B C
```
*represents a rule such that A is the left-side of the rule and BC is the right-side of the rule.*

After getting the graph and grammar file into the **graspan-cpp/src** folder, run the **run.sh** script in your command line specifying, 

1. the graph file,
2. the grammar file
3. the number of partitions user would like to generate from the graph during preprocessing, prior to computation,
4. the memory budget: the user’s preferred memory budget (in GB). It represents an upper bound of memory for Graspan to use for the entire computation
5. the number of threads: the number of threads the user would like the program to use. The recommended value is twice the number of  available cores.

as shown below, 

```
./run  <graph_file>  <grammar_file>  <number_partitions>  <memory_budget>  <num_threads>
```

After running the above command, you can monitor the progress of the computation by viewing the generated **cpp.x.output** file in the **graspan-cpp/src/log** folder. After computation ends, this file will show the number of edges generated and the total computation time. The **.partition.** output files will contain the partitioned graph with new edges.

## Questions or Comments?
Participate in our discussion group, [GraspanMeet](https://groups.google.com/d/forum/graspanmeet).

## Project Contributors

* [**Kai Wang**](http://web.cs.ucla.edu/~wangkai/) - *PhD Student, UCLA* 
* [**Aftab Hussain**](http://www.ics.uci.edu/~aftabh/) - *PhD Student, UCI* 
* [**Zhiqiang Zuo**](http://z-zhiqiang.github.io/) - *Assistant Professor, Nanjing University* 
* [**Harry Xu**](http://www.cs.ucla.edu/~harryxu) - *Associate Professor, UCLA* 
* [**Ardalan Sani**](http://www.ics.uci.edu/~ardalan/) - *Assistant Professor, UCI* 
* **John Thorpe** - *PhD Student, UCLA*
* **Sung-Soo Son** - *Visiting Undergraduate Student, UCI*
* [**Khanh Ngyuen**](https://khanhtn1.bitbucket.io/) - *Assistant Professor, Texas A&M University*
