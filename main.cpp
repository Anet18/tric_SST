// ***********************************************************************
//
//                              TRIC
//
// ***********************************************************************
//
//       Copyright (2019) Battelle Memorial Institute
//                      All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// ************************************************************************ 



#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <cassert>
#include <cstdlib>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <mpi.h>

#include "ibufastric.hpp"

int main(int argc, char *argv[])
{
  double t0, t1, td, td0, td1;

  std::string inputFileName;
  int me, nprocs;
  int ranksPerNode = 1;
  GraphElem nvRGG = 0;
  int generateGraph = 0;
  bool readBalanced = false;
  GraphWeight randomEdgePercent = 0.0;
  bool randomNumberLCG = false;
  bool estimateTriangles = false;
  bool bufferSet = false;
  long bufferSize = -1;

  MPI_Init(&argc, &argv);

  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  MPI_Comm_rank(MPI_COMM_WORLD, &me);

  // command line options
  int ret;
  optind = 1;

  while ((ret = getopt(argc, argv, "f:r:n:p:olbs:")) != -1) {
    switch (ret) {
    case 'f':
        inputFileName.assign(optarg);
        break;
      case 'b':
        readBalanced = true;
        break;
      case 'r':
        ranksPerNode = atoi(optarg);
        break;
      case 'n':
        nvRGG = atol(optarg);
        if (nvRGG > 0)
          generateGraph = true; 
        break;
      case 'l':
        randomNumberLCG = true;
        break;
      case 'p':
        randomEdgePercent = atof(optarg);
        break;
      case 'o':
        estimateTriangles = true;
        break;
      case 's':
        bufferSet = true;
        bufferSize = atol(optarg);
        break;
      default:
        assert(0 && "Should not reach here!!");
        break;
    }
  }
  // warnings/info

  if (me == 0 && generateGraph && readBalanced) 
  {
    std::cout << "Balanced read (option -b) is only applicable for real-world graphs. "
      << "This option does nothing for generated (synthetic) graphs." << std::endl;
  } 

  // errors
  if (me == 0 && (argc == 1)) 
  {
    std::cerr << "Must specify some options." << std::endl;
    MPI_Abort(MPI_COMM_WORLD, -99);
  }

  if (me == 0 && !generateGraph && inputFileName.empty()) 
  {
    std::cerr << "Must specify a binary file name with -f or provide parameters for generating a graph." << std::endl;
    MPI_Abort(MPI_COMM_WORLD, -99);
  }

  if (me == 0 && !generateGraph && randomNumberLCG) 
  {
    std::cerr << "Must specify -g for graph generation using LCG." << std::endl;
    MPI_Abort(MPI_COMM_WORLD, -99);
  }

  if (me == 0 && !generateGraph && (randomEdgePercent > 0.0)) 
  {
    std::cerr << "Must specify -g for graph generation first to add random edges to it." << std::endl;
    MPI_Abort(MPI_COMM_WORLD, -99);
  } 

  if (me == 0 && generateGraph && ((randomEdgePercent < 0.0) || (randomEdgePercent >= 100.0))) 
  {
    std::cerr << "Invalid random edge percentage for generated graph!" << std::endl;
    MPI_Abort(MPI_COMM_WORLD, -99);
  }

  Graph* g = nullptr;

  td0 = MPI_Wtime();

  // generate graph only supports RGG as of now
  if (generateGraph) 
  {
    if (!is_pwr2(nprocs)) 
    {
      std::cout << "Error: random geometric graph generation require power-of-2 #processes." << std::endl;
      MPI_Abort(MPI_COMM_WORLD, -99);
    }
    GenerateRGG gr(nvRGG);
    g = gr.generate(randomNumberLCG, true /*isUnitEdgeWeight*/, randomEdgePercent);
  }
  else 
  {   // read input graph
    BinaryEdgeList rm;
    if (readBalanced == true)
    {
      if (me == 0)
      {
        std::cout << std::endl;
        std::cout << "Trying to balance the edge distribution while reading: " << std::endl;
        std::cout << inputFileName << std::endl;
      }
      g = rm.read_balanced(me, nprocs, ranksPerNode, inputFileName);
    }
    else
      g = rm.read1(me, nprocs, ranksPerNode, inputFileName);
  }

#if defined(PRINT_GRAPH_EDGES)        
  g->print();
#endif
  g->print_dist_stats();
  assert(g != nullptr);

  MPI_Barrier(MPI_COMM_WORLD);
#ifdef DEBUG_PRINTF  
  assert(g);
#endif
  td1 = MPI_Wtime();
  td = td1 - td0;

  double tdt = 0.0;
  MPI_Reduce(&td, &tdt, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

  if (me == 0)  
  {
    if (!generateGraph)
      std::cout << "Time to read input file and create distributed graph (secs.): " 
        << tdt << std::endl;
    else
      std::cout << "Time to generate distributed graph of " 
        << nvRGG << " vertices (secs.): " << tdt << std::endl;
  }

  MPI_Barrier(MPI_COMM_WORLD);

  if (bufferSize < 100)
    bufferSize = DEFAULT_BUF_SIZE;
  
  TriangulateAggrBufferedIrecv tr(g, bufferSize);


  MPI_Barrier(MPI_COMM_WORLD);

  t0 = MPI_Wtime();
  GraphElem ntris = tr.count();
  MPI_Barrier(MPI_COMM_WORLD);
  t1 = MPI_Wtime();
  double p_tot = t1 - t0, t_tot = 0.0;

  MPI_Reduce(&p_tot, &t_tot, 1, MPI_DOUBLE, 
      MPI_SUM, 0, MPI_COMM_WORLD);
  if (me == 0) 
  {   double avg_t = (double)(t_tot/(double)nprocs);
    std::cout << "Average execution time (secs.) for distributed counting on " << nprocs << " processes: " 
      << avg_t << std::endl;

    std::cout << "Number of triangles: " << ntris << std::endl;
    std::cout << "TEPS: " << g->get_ne()/avg_t << std::endl;
    //std::cout << "Resolution of MPI_Wtime: " << MPI_Wtick() << std::endl;
  }

  tr.clear(); 
  
  MPI_Barrier(MPI_COMM_WORLD);

  MPI_Finalize();

  return 0;
}
