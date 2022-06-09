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
#pragma once
#ifndef CHASH_TFASTRIC_HPP
#define CHASH_TFASTRIC_HPP

#include "graph.hpp"

#include <numeric>
#include <utility>
#include <cstring>
#include <iomanip>
#include <limits>

#ifndef TAG_DATA
#define TAG_DATA 100
#endif

#ifndef BLOOMFILTER_TOL
#define BLOOMFILTER_TOL 1E-09
#endif

#include "murmurhash/MurmurHash3.h"

class Bloomfilter
{
  public:
    Bloomfilter(GraphElem n, GraphWeight p=BLOOMFILTER_TOL) 
      : n_(pow(2, std::ceil(log(n)/log(2)))), p_(p)
    {
      m_ = std::ceil((n_ * log(p_)) / log(1 / pow(2, log(2))));
      k_ = std::round((m_ / n_) * log(2));

      hashes_.resize(k_); 
      bits_.resize(m_);
      std::fill(bits_.begin(), bits_.end(), '0');

      if (k_ == 0)
        throw std::invalid_argument("Bloomfilter could not be initialized: k must be larger than 0");
    }
        
    Bloomfilter(GraphElem n, GraphElem k, GraphWeight p) 
      : n_(pow(2, std::ceil(log(n)/log(2)))), k_(k), p_(p)
    {
      m_ = std::ceil((n_ * log(p_)) / log(1 / pow(2, log(2))));

      if (k_%2 != 0)
        k_ += 1;

      hashes_.resize(k_); 
      bits_.resize(m_);
      std::fill(bits_.begin(), bits_.end(), '0');

      if (k_ == 0)
        throw std::invalid_argument("Bloomfilter could not be initialized: k must be larger than 0");
    }

    void insert(GraphElem const& i, GraphElem const& j)
    {
      hash(i, j);
      for (GraphElem k = 0; k < k_; k++)
        bits_[hashes_[k]] = '1';
    }

    void print() const
    {
      std::cout << "-------------Bloom filter statistics-------------" << std::endl;
      std::cout << "Number of Items (n): " << n_ << std::endl;
      std::cout << "Probability of False Positives (p): " << p_ << std::endl;
      std::cout << "Number of bits in filter (m): " << m_ << std::endl;
      std::cout << "Number of hash functions (k): " << k_ << std::endl;
      std::cout << "-------------------------------------------------" << std::endl;
    }

    void clear()
    {
        bits_.clear(); 
        hashes_.clear(); 
    }

    bool contains(GraphElem i, GraphElem j) 
    {
      hash(i, j);
      for (GraphElem k = 0; k < k_; k++)
      {
        if (bits_[hashes_[k]] == '0') 
          return false;
      }
      return true;
    }

    GraphElem nbits() const
    { return m_; }

    char const* data() const
    { return bits_.data(); }

    // "nucular" options, use iff 
    // you know what you're doing
    void copy_from(char* dest)
    { std::memcpy(dest, bits_.data(), m_); }
      
    void copy_to(char* source)
    { std::memcpy(bits_.data(), source, m_); }
     
    void copy_from(char* dest, ptrdiff_t offset)
    { std::memcpy(dest, &bits_[offset], m_); }
      
    void copy_to(char* source, ptrdiff_t offset)
    { std::memcpy(&bits_[offset], source, m_); }   
    
    char* data()
    { return bits_.data(); }

  private:
    GraphElem n_, m_, k_;
    GraphWeight p_;

    void hash( uint64_t lhs, uint64_t rhs ) 
    {
      uint64_t key[2] = {lhs, rhs};
      for (uint64_t n = 0; n < k_; n+=2)
      {
        MurmurHash3_x64_128 ( &key, 2*sizeof(uint64_t), 0, &hashes_[n] );
        hashes_[n] = hashes_[n] % m_; 
        hashes_[n+1] = hashes_[n+1] % m_;
      }
    }
    
    std::vector<char> bits_;
    std::vector<uint64_t> hashes_;
};

class TriangulateHashRemote
{
  public:

    TriangulateHashRemote(Graph* g): 
      g_(g), pdegree_(0), erange_(nullptr), ntriangles_(0), pindex_(0), 
      sebf_(nullptr), rebf_(nullptr), targets_(0)
  {
    comm_ = g_->get_comm();
    MPI_Comm_size(comm_, &size_);
    MPI_Comm_rank(comm_, &rank_);

    const GraphElem lnv = g_->get_lnv();
    const GraphElem nv = g_->get_nv();
    
    erange_ = new GraphElem[nv*2]();
    std::vector<int> vtargets; 
    std::vector<std::vector<int>> vcount(lnv);

    // store edge ranges
    GraphElem base = g_->get_base(rank_);
    for (GraphElem i = 0; i < lnv; i++)
    {
      GraphElem e0, e1;
      g_->edge_range(i, e0, e1);

      if ((e0 + 1) == e1)
        continue;
      
      for (GraphElem m = e0; m < e1; m++)
      {
        Edge const& edge_m = g_->get_edge(m);
        const int owner = g_->get_owner(edge_m.tail_);
        if (owner != rank_)
        {
          if (std::find(vtargets.begin(), vtargets.end(), owner) 
              == vtargets.end())
            vtargets.push_back(owner);
        }
      }

      vcount[i].insert(vcount[i].end(), vtargets.begin(), vtargets.end());      
      vtargets.clear();

      Edge const& edge_s = g_->get_edge(e0);
      Edge const& edge_t = g_->get_edge(e1-1);

      erange_[(i + base)*2] = edge_s.tail_;
      erange_[(i + base)*2+1] = edge_t.tail_;
    }
    
    MPI_Barrier(comm_);
    
    MPI_Allreduce(MPI_IN_PLACE, erange_, nv*2, MPI_GRAPH_TYPE, 
        MPI_SUM, comm_);

    GraphElem *send_count  = new GraphElem[size_]();
    GraphElem *recv_count  = new GraphElem[size_]();

    double t0 = MPI_Wtime();

    GraphElem nedges = 0;

    // perform local counting, identify edges and store targets 
    for (GraphElem i = 0; i < lnv; i++)
    {
      GraphElem e0, e1, tup[2];
      g_->edge_range(i, e0, e1);

      if ((e0 + 1) == e1)
        continue;

      for (GraphElem m = e0; m < e1; m++)
      {
        Edge const& edge_m = g_->get_edge(m);
        const int owner = g_->get_owner(edge_m.tail_);

        if (owner != rank_)
        {  
          if (std::find(targets_.begin(), targets_.end(), owner) 
              == targets_.end())
            targets_.push_back(owner);
          
          nedges += vcount[i].size();
          for (int p : vcount[i])
            send_count[p] += 1;
        }
        else
        {
          if (m < (e1 - 1))
          {
            tup[0] = edge_m.tail_;
            for (GraphElem n = m + 1; n < e1; n++)
            {
              Edge const& edge_n = g_->get_edge(n);
              tup[1] = edge_n.tail_;

              if (check_edgelist(tup))
                ntriangles_ += 1;
            }
          }
          
          int past_target = -1;
          GraphElem l0, l1;
          const GraphElem lv = g_->global_to_local(edge_m.tail_);
          g_->edge_range(lv, l0, l1);
          
          for (GraphElem l = l0; l < l1; l++)
          {
            Edge const& edge = g_->get_edge(l);
            const int target = g_->get_owner(edge.tail_);
            if (target != rank_)
            {
              if (target != past_target)
              {
                send_count[target] += 1;
                nedges += 1;
                past_target = target;
              }
            }
          }
        }
      }
    }

    assert(nedges == std::accumulate(send_count, send_count + size_, 0));
    
    // outgoing/incoming data and buffer size
    MPI_Alltoall(send_count, 1, MPI_GRAPH_TYPE, recv_count, 1, MPI_GRAPH_TYPE, comm_);
     
    MPI_Barrier(comm_);

    double t1 = MPI_Wtime();
    double p_tot = t1 - t0, t_tot = 0.0;

    MPI_Reduce(&p_tot, &t_tot, 1, MPI_DOUBLE, MPI_SUM, 0, comm_);

    if (rank_ == 0) 
    {   
      std::cout << "Average time for local counting during instantiation (secs.): " 
        << ((double)(t_tot / (double)size_)) << std::endl;
    }

    // neighbor topology
    MPI_Dist_graph_create_adjacent(comm_, targets_.size(), targets_.data(), 
        MPI_UNWEIGHTED, targets_.size(), targets_.data(), MPI_UNWEIGHTED, 
        MPI_INFO_NULL, 0 /*reorder ranks?*/, &gcomm_);

    pdegree_ = targets_.size();

    for (int i = 0; i < pdegree_; i++)
      pindex_.insert({targets_[i], i});
  
    sebf_ = new Bloomfilter*[pdegree_]; 
    rebf_ = new Bloomfilter*[pdegree_]; 

    std::vector<GraphElem> scounts(pdegree_,0), rcounts(pdegree_,0);
#if defined(USE_ALLTOALLV)
    GraphElem sdisp = 0, rdisp = 0;
#endif
    for (GraphElem p = 0; p < size_; p++)
    {
      if (send_count[p] > 0)
      {
        sebf_[pindex_[p]] = new Bloomfilter(send_count[p]*2);
        scounts[pindex_[p]] = sebf_[pindex_[p]]->nbits();
#if defined(USE_ALLTOALLV)
        sdisp += scounts[pindex_[p]];
#endif
      }

      if (recv_count[p] > 0)
      {
        rebf_[pindex_[p]] = new Bloomfilter(recv_count[p]*2);
        rcounts[pindex_[p]] = rebf_[pindex_[p]]->nbits();
#if defined(USE_ALLTOALLV)
        rdisp += rcounts[pindex_[p]];
#endif
      }
    }
    
    t0 = MPI_Wtime();

    MPI_Barrier(comm_);
  
    // store edges in bloomfilter
    for (GraphElem i = 0; i < lnv; i++)
    {
      GraphElem e0, e1, tup[2];
      g_->edge_range(i, e0, e1);

      if ((e0 + 1) == e1)
        continue;

      for (GraphElem m = e0; m < e1; m++)
      {
        Edge const& edge_m = g_->get_edge(m);
        const int owner = g_->get_owner(edge_m.tail_);

        if (owner != rank_)
        {
          for (int p : vcount[i])
            sebf_[pindex_[p]]->insert(g_->local_to_global(i), edge_m.tail_);
        }
        else
        {
          int past_target = -1;
          GraphElem l0, l1;
          const GraphElem lv = g_->global_to_local(edge_m.tail_);
          g_->edge_range(lv, l0, l1);
          for (GraphElem l = l0; l < l1; l++)
          {
            Edge const& edge = g_->get_edge(l);
            const int target = g_->get_owner(edge.tail_);
            if (target != rank_)
            {
              if (target != past_target)
              {
                sebf_[pindex_[target]]->insert(g_->local_to_global(i), edge_m.tail_);
                past_target = target;
              }
            }
          }
        }
      }
    }
 
    MPI_Barrier(comm_);
   
#if defined(USE_ALLTOALLV) 
    char *sbuf = new char[sdisp];
    char *rbuf = new char[rdisp];
#else
    MPI_Request *reqs = new MPI_Request[pdegree_*2];
    std::fill(reqs, reqs + pdegree_*2, MPI_REQUEST_NULL);
#endif
      

    // batched communication
    int nbatches = 1;
    GraphElem max_send_count = *std::max_element(scounts.begin(), scounts.end());
    MPI_Allreduce(MPI_IN_PLACE, &max_send_count, 1, MPI_GRAPH_TYPE, MPI_MAX, comm_);
    GraphElem batch_size = (GraphElem)std::numeric_limits<int>::max();
    
    while(batch_size < max_send_count)
    {
      nbatches += 1;
      batch_size += (GraphElem)std::numeric_limits<int>::max();
    }

    if (rank_ == 0)
      std::cout << "Number of batches: " << nbatches << std::endl;

    int *batch_send_counts = new int[pdegree_*nbatches];
    int *batch_recv_counts = new int[pdegree_*nbatches];
    std::memset(batch_send_counts, 0, pdegree_*nbatches*sizeof(int));

    for (int p = 0; p < pdegree_; p++)
    {
      for (int i = 0; i < nbatches; i++)
      {
        batch_send_counts[p*nbatches+i] = MIN(std::numeric_limits<int>::max(), scounts[p]);
        scounts[p] -= batch_send_counts[p*nbatches+i];
      }
    }

    MPI_Barrier(comm_);

    MPI_Neighbor_alltoall(batch_send_counts, nbatches, MPI_INT, batch_recv_counts, nbatches, MPI_INT, gcomm_);
      
#ifdef USE_ALLTOALLV
    std::vector<int> rcnts(pdegree_,0), scnts(pdegree_,0);
    std::vector<int> rdispls(pdegree_,0), sdispls(pdegree_,0);
#endif

    GraphElem spos = 0, rpos = 0;

    for (int n = 0; n < nbatches; n++)
    { 
#ifdef USE_ALLTOALLV
      for (int p = 0; p < pdegree_; p++)
      {
        sdispls[p] = (int)spos;
        rdispls[p] = (int)rpos;
        scnts[p] = batch_send_counts[p*nbatches+n];
        rcnts[p] = batch_recv_counts[p*nbatches+n];
        spos += scnts[p];
        rpos += rcnts[p];
      }
    
      for(GraphElem p = 0; p < pdegree_; p++)
      {
        if (scnts[p] > 0)
          sebf_[p]->copy_from(&sbuf[sdispls[p]], n*spos);
      }

      MPI_Neighbor_alltoallv(sbuf, scnts.data(), sdispls.data(), MPI_CHAR, rbuf, rcnts.data(), rdispls.data(), MPI_CHAR, gcomm_);

      for(int p = 0; p < pdegree_; p++)
      {
        if (rcnts[p] > 0)
          rebf_[p]->copy_to(&rbuf[rdispls[p]], n*rpos);
      }
#else
      for (int p = 0; p < pdegree_; p++)
      {
        MPI_Irecv(rebf_[p]->data() + n*rpos, batch_recv_counts[p*nbatches+n], MPI_CHAR, targets_[p], TAG_DATA, comm_, &reqs[p]);
        rpos += batch_recv_counts[p*nbatches+n];
      }

      for (int p = 0; p < pdegree_; p++)
      {
        MPI_Isend(sebf_[p]->data() + n*spos, batch_send_counts[p*nbatches+n], MPI_CHAR, targets_[p], TAG_DATA, comm_, &reqs[p+pdegree_]);
        spos += batch_send_counts[p*nbatches+n];
      }

      MPI_Waitall(pdegree_*2, reqs, MPI_STATUSES_IGNORE);
#endif
      MPI_Barrier(comm_);
    } // end of batches

#if defined(DEBUG_PRINTF)
    if (rank_ == 0)
    {
      std::cout << "Edge range per vertex (#ID: <range>): " << std::endl;
      for (int i = 0, j = 0; i < nv*2; i+=2, j++)
        std::cout << j << ": " << erange_[i] << ", " << erange_[i+1] << std::endl;
    }
#endif
    
    delete []send_count;
    delete []recv_count;
    delete []batch_send_counts;
    delete []batch_recv_counts;

#if defined(USE_ALLTOALLV) 
    delete []sbuf;
    delete []rbuf;
#else
    delete []reqs;
#endif

    for (int i = 0; i < lnv; i++)
      vcount[i].clear();
    vcount.clear();
 
    scounts.clear();
    rcounts.clear();
  }

    ~TriangulateHashRemote() {}

    void clear()
    {
      MPI_Comm_free(&gcomm_);
      
      if (rebf_ && sebf_)
      {
        for (int p = 0; p < pdegree_; p++)
        {
          rebf_[p]->clear();
          sebf_[p]->clear();
        }

        delete []rebf_;
        delete []sebf_;
      }

      pindex_.clear();
      targets_.clear();
      delete []erange_;
    }

    inline GraphElem count()
    {
      const GraphElem lnv = g_->get_lnv();

      for (GraphElem i = 0; i < lnv; i++)
      {
        GraphElem e0, e1;
        g_->edge_range(i, e0, e1);

        if ((e0 + 1) == e1)
          continue;

        for (GraphElem m = e0; m < e1-1; m++)
        {
          Edge const& edge_m = g_->get_edge(m);
          const int owner = g_->get_owner(edge_m.tail_);
          const GraphElem pidx = pindex_[owner];

          if (owner != rank_)
          {
            for (GraphElem n = m + 1; n < e1; n++)
            {
              Edge const& edge_n = g_->get_edge(n);
                
              if (!edge_within_max(edge_m.tail_, edge_n.tail_))
                break;
              if (!edge_above_min(edge_m.tail_, edge_n.tail_) || !edge_above_min(edge_n.tail_, edge_m.tail_))
                continue;

              if (rebf_[pidx]->contains(edge_m.tail_, edge_n.tail_))
              {
                ntriangles_ += 1;
              }
            }
          }
        }
      }
      
      GraphElem ttc = 0, ltc = ntriangles_;
      MPI_Barrier(comm_);
      MPI_Reduce(&ltc, &ttc, 1, MPI_GRAPH_TYPE, MPI_SUM, 0, comm_);

      return (ttc / 3);
    }

    inline bool check_edgelist(GraphElem tup[2])
    {
      GraphElem e0, e1;
      const GraphElem lv = g_->global_to_local(tup[0]);
      g_->edge_range(lv, e0, e1);
      for (GraphElem e = e0; e < e1; e++)
      {
        Edge const& edge = g_->get_edge(e);
        if (tup[1] == edge.tail_)
          return true;
        if (edge.tail_ > tup[1]) 
          break;
      }
      return false;
    }
    
    inline bool edge_between_range(GraphElem x, GraphElem y) const
    {
      if ((y >= erange_[x*2]) && (y <= erange_[x*2+1]))
        return true;
      return false;
    }
     
    inline bool edge_above_min(GraphElem x, GraphElem y) const
    {
      if (y >= erange_[x*2])
        return true;
      return false;
    }

    inline bool edge_within_max(GraphElem x, GraphElem y) const
    {
      if (y <= erange_[x*2+1])
        return true;
      return false;
    }

  private:
    Graph* g_;

    GraphElem ntriangles_, pdegree_;
    GraphElem *erange_;
    Bloomfilter **sebf_, **rebf_;

    std::vector<int> targets_;

    int rank_, size_;
    std::unordered_map<int, int> pindex_; 
    MPI_Comm comm_, gcomm_;
};
#endif
