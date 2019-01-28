#include <thread>
#include <mutex>
#include "siever.h"

// reserves size for db and cdb. If called with a larger size than the current capacities, does nothing
void Siever::reserve(size_t const reserved_db_size)
{
    CPUCOUNT(216);
    db.reserve(reserved_db_size);
    cdb.reserve(reserved_db_size);
    // bgj1_cdb_copy is only needed for bgj1. We delay the reservation until we need it.
    if( sieve_status == SieveStatus::bgj1 ) bgj1_cdb_copy.reserve(reserved_db_size);
}

// switches the current sieve_status to the new one and updates internal data accordingly.
// Note that, depending on the sieve_status, we keep track of the sortedness of cdb in a different way.
// (because for some sieves this encodes work that is already done and we want to keep this across calls)
bool Siever::switch_mode_to(Siever::SieveStatus new_sieve_status)
{
    CPUCOUNT(217);
    static_assert(static_cast<int>(Siever::SieveStatus::LAST) == 4, "You need to update this function");
    if (new_sieve_status == sieve_status)
    {
        return false;
    }
    switch(new_sieve_status)
    {
        case SieveStatus::bgj1 :
            bgj1_cdb_copy.reserve(db.capacity());
            [[fallthrough]];
        case SieveStatus::plain :
            if(sieve_status == SieveStatus::gauss || sieve_status == SieveStatus::triple_mt)
            {
                StatusData::Gauss_Data &data = status_data.gauss_data;
                StatusData new_status_data;
                assert(data.list_sorted_until <= data.queue_start);
                assert(data.queue_start       <= data.queue_sorted_until);
                assert(data.queue_sorted_until<= db_size() );
                assert(std::is_sorted(cdb.cbegin(),cdb.cbegin()+data.list_sorted_until, &compare_CE  ) );
                assert(std::is_sorted(cdb.cbegin()+data.queue_start, cdb.cbegin() + data.queue_sorted_until , &compare_CE  ));
                if(data.list_sorted_until == data.queue_start)
                {
                    std::inplace_merge(cdb.begin(), cdb.begin()+ data.queue_start, cdb.begin()+ data.queue_sorted_until, &compare_CE);
                    new_status_data.plain_data.sorted_until =  data.queue_sorted_until;
                }
                else
                {
                    new_status_data.plain_data.sorted_until = data.list_sorted_until;
                }
                status_data = new_status_data;
            }
            // switching between bgj1 and plain does nothing, essentially.
            sieve_status = new_sieve_status;
            break;
        // switching from gauss to triple_mt and vice-versa is ill-supported (it works, but it throws away work)
        case SieveStatus::gauss :
            [[fallthrough]];
        case SieveStatus::triple_mt :
            if(sieve_status == SieveStatus::plain || sieve_status == SieveStatus::bgj1)
            {
                StatusData::Plain_Data &data = status_data.plain_data;
                assert(data.sorted_until <= cdb.size());
                assert(std::is_sorted(cdb.cbegin(), cdb.cbegin() + data.sorted_until, &compare_CE ));
                StatusData new_status_data;
                new_status_data.gauss_data.list_sorted_until = 0;
                new_status_data.gauss_data.queue_start = 0;
                new_status_data.gauss_data.queue_sorted_until = data.sorted_until;
                status_data = new_status_data;
            }
            sieve_status = new_sieve_status;
            break;

        default : assert(false); break;
    }
    return true;
}

// use to indicate that the cdb is no longer sorted. In case of gauss sieves that distinguish between list and queue,
// this also sets the length of the list part to 0.
void Siever::invalidate_sorting()
{
    static_assert(static_cast<int>(Siever::SieveStatus::LAST) == 4, "You need to update this function");
    switch(sieve_status)
    {
        case SieveStatus::plain :
            [[fallthrough]];
        case SieveStatus::bgj1 :
            status_data.plain_data.sorted_until = 0;
            break;
        case SieveStatus::gauss :
            [[fallthrough]];
        case SieveStatus::triple_mt :
            status_data.gauss_data.list_sorted_until = 0;
            status_data.gauss_data.queue_start = 0;
            status_data.gauss_data.queue_sorted_until = 0;
            status_data.gauss_data.reducedness = 0;
            break;
        default: assert(false); break;
    }
}

// indicates that histo is invalid.
void Siever::invalidate_histo()
{
    histo_valid = false;
}

// Sets the number of threads our threadpool uses + master thread.
// Note that the threadpool itself uses one less thread, because there is also a master thread.
void Siever::set_threads(unsigned int nr)
{
    assert(nr >= 1);
    threadpool.resize(nr-1);
}

// Loads (full) gso of size full_n. The GSO matrix is passed as an one-dim C-Array.
// Note that this is called from the python layer with an already existing (c)db
// (GSO Updates are breaking encapsulation at the moment)
void Siever::load_gso(unsigned int full_n, double const* mu)
{
    this->full_n = full_n;
    full_muT.resize(full_n);
    full_rr.resize(full_n);

    for (unsigned int i = 0; i < full_n; ++i)
    {
        full_muT[i].resize(full_n);
        for (unsigned int j = 0; j < full_n; ++j)
        {
            full_muT[i][j] = mu[j * full_n + i];
        }
    }
    for (unsigned int i = 0; i < full_n; ++i)
    {
        full_rr[i] = full_muT[i][i];
        full_muT[i][i] = 1;
    }
    invalidate_sorting();
    invalidate_histo();
}

// initializes a local block from [l_,  r_) from the full GSO object.
// This has to be called before we start sieving.
void Siever::initialize_local(unsigned int l_, unsigned int r_)
{
    CPUCOUNT(200);

    assert(r_ >= l_);
    assert(full_n >= r_);
    // r stays same or increases => keep best lifts
    if (r_ >= r)
    {
        best_lifts_so_far.resize(l_+1);
        for (auto& bl : best_lifts_so_far)
        {
            if (bl.len == 0.) continue;
            bl.x.resize(r_, 0);

            if (!params.lift_unitary_only) continue;
            bool unitary=false;
            for (int i = l_; i < r_; ++i)
            {
                unitary |= abs(bl.x[i])==1;
            }
            if (unitary) continue;
            bl.x.clear();
            bl.len = 0;
        }
    }
    // r shrinked
    if (r_ < r)
    {
        best_lifts_so_far.clear();
        best_lifts_so_far.resize(l_+1);
    }


    l = l_;
    r = r_;
    n = r_ - l_;

    // std::fill(histo.begin(), histo.end(), 0);
    invalidate_histo();

    //mu.resize(n);
    muT.resize(n);
    rr.resize(n);
    sqrt_rr.resize(n);

    for (unsigned int i = 0; i < n; ++i)
    {
        muT[i].resize(n);
        for (unsigned int j = 0; j < n; ++j)
        {
            muT[i][j] = full_muT[i + l][j + l];
        }
        rr[i] = full_rr[i + l];
        // Note: rr will get normalized by gh below
        // sqrt_rr is set below after normalization
    }

    // Compute the Gaussian Heuristic of the current block
    double const log_ball_square_vol = n * std::log(M_PI) - 2.0 * std::lgamma(n / 2.0 + 1);
    double log_lattice_square_vol = 0;
    for (unsigned int i = 0; i < n; ++i)
    {
        log_lattice_square_vol += std::log(rr[i]);
    }
    gh = std::exp((log_lattice_square_vol - log_ball_square_vol) / (1.0 * n));

    // Renormalize local rr coefficients
    for (unsigned int i = 0; i < n; ++i)
    {
        rr[i] /= gh;
        sqrt_rr[i] = std::sqrt(rr[i]);
    }

    set_lift_bounds();
    sim_hashes.reset_compress_pos(*this);
    uid_hash_table.reset_hash_function(*this);
    invalidate_sorting();
}

// This is run internally after a change of context / GSO.
// It assumes that the uid_hash table is empty and re-inserts every (c)db element into it.
// If collisions are found, they are replaced by fresh samples.
void Siever::refresh_db_collision_checks()
{
    CPUCOUNT(201);

    // db_uid.clear(); initialize_local calls uid_hash_table.reset_hash_function(), which clears the uid database.
    // initialize_local is always called before this function.
    // updated.clear();
    // Run collision checks
    assert(uid_hash_table.hash_table_size() == 0);

    apply_to_all_compressed_entries([this](CompressedEntry &ce)
    {
        // the cdb is not in a valid state, so do not sample by recombination
        int retry = 1;
        while (!uid_hash_table.insert_uid(db[ce.i].uid))
        {
            db[ce.i] = std::move(sample(retry));
            retry += 1;
        }
        ce.len = db[ce.i].len;
        ce.c = db[ce.i].c;
    } );
    invalidate_sorting();
    invalidate_histo();
}


// Increase the current block to the left by lp coefficients, using babai lifting
// Pretty much everything needs to be recomputed
void Siever::extend_left(unsigned int lp)
{
    CPUCOUNT(202);

    assert(lp <= l);
    initialize_local(l - lp, r);

    apply_to_all_entries([lp,this](Entry &e)
        {
          // Padding with lp entries from the left. Note that these will be overwritten by the
          // babai_lifting done in recompute_data_for_entry_babai.
          // e.yr is padded from the right, but it's completely recomputed anyway.
          std::copy_backward(e.x.begin(), e.x.begin()+n-lp, e.x.begin()+n);
          std::fill(e.x.begin(), e.x.begin()+lp, 0);
          std::fill(e.x.begin()+n,e.x.end(),0);
          recompute_data_for_entry_babai<Recompute::babai_only_needed_coos_and_recompute_aggregates | Recompute::recompute_yr>(e,lp);
        } );
    invalidate_sorting();
    invalidate_histo();
    refresh_db_collision_checks();
}

void Siever::shrink_left(unsigned int lp)
{
    CPUCOUNT(204);
    initialize_local(l + lp , r);
    apply_to_all_entries([lp,this](Entry &e)
        {
            std::copy(e.x.begin()+lp,e.x.begin()+lp+n,e.x.begin());
            std::fill(e.x.begin()+n,e.x.end(),0);
            recompute_data_for_entry<Recompute::recompute_yr | Recompute::recompute_len | Recompute::recompute_c | Recompute::recompute_uid | Recompute::consider_otf_lift | Recompute::recompute_otf_helper >(e);
        } );
    invalidate_sorting();
    invalidate_histo();
    refresh_db_collision_checks();
}

// Increase the current block to the left by lp coefficients, appending zeros.
void Siever::extend_right(unsigned int rp)
{
    CPUCOUNT(203);

    initialize_local(l, r + rp);

    apply_to_all_entries([rp,this](Entry &e)
                          {
                            std::fill(e.x.begin()+n,e.x.end(),0);
                            recompute_data_for_entry<Recompute::recompute_yr | Recompute::recompute_len | Recompute::recompute_c | Recompute::recompute_uid | Recompute::consider_otf_lift | Recompute::recompute_otf_helper >(e);
                          } );
    refresh_db_collision_checks();
    invalidate_sorting(); // False positive hash collisions can actually invalidate sorting atm.
    invalidate_histo();
}

template<int tnold>
void Siever::gso_update_postprocessing_task(size_t const start, size_t const end, int const n_old, std::vector<std::array<ZT,MAX_SIEVING_DIM>> const &MT)
{
    ATOMIC_CPUCOUNT(218);
    assert(n_old == (tnold < 0 ? n_old : tnold));
    std::array<ZT,MAX_SIEVING_DIM> x_new; // create one copy on the stack to avoid reallocating memory inside the loop.

    for(size_t i = start; i < end; ++i)
    {
        std::fill(x_new.begin(), x_new.end(), 0);
        for(unsigned int j = 0; j < n; ++j)
        {
            x_new[j] = std::inner_product(db[i].x.begin(), db[i].x.begin()+(tnold<0?n_old:tnold), MT[j].begin(), static_cast<ZT>(0));
        }
        db[i].x = x_new;
        recompute_data_for_entry<Recompute::recompute_all_and_consider_otf_lift>(db[i]);
    }
}

void Siever::gso_update_postprocessing(const unsigned int l_, const unsigned int r_, long const * M)
{
    CPUCOUNT(208);

    const unsigned int n_old = n;

    // save old best lifts in old basis
    std::vector<Entry> oldbestlifts;
    for (auto& bl: best_lifts_so_far)
    {
        if (bl.len<=0.) continue;
        oldbestlifts.emplace_back();
        for (unsigned int j = 0; j < n; ++j)
            oldbestlifts.back().x[j] = bl.x[l + j];
    }

    best_lifts_so_far.clear();
    best_lifts_so_far.resize(l_+1);

    initialize_local(l_, r_);

    std::vector<std::array<ZT,MAX_SIEVING_DIM>> MT;
    MT.resize(n);
    for (unsigned int i = 0; i < n; ++i)
        std::copy(M+(i*n_old), M+(i*n_old)+n_old, MT[i].begin());

    // retry lifting old best lifts under new basis
    for (auto& e : oldbestlifts)
    {
        auto x_new = e.x;
        std::fill(x_new.begin(), x_new.end(), 0);
        for(unsigned int j = 0; j < n; ++j)
        {
            x_new[j] = std::inner_product(e.x.begin(), e.x.begin()+n_old, MT[j].begin(), static_cast<ZT>(0));
        }

        e.x = x_new;
        recompute_data_for_entry<Recompute::recompute_all_and_consider_otf_lift>(e);
    }

    auto task = &Siever::gso_update_postprocessing_task<-1>;
    UNTEMPLATE_DIM(&Siever::gso_update_postprocessing_task, task, n_old);

    size_t const th_n = std::min(params.threads, static_cast<size_t>(1 + db.size() / MIN_ENTRY_PER_THREAD));

    for (size_t c = 0; c < th_n; ++c)
    {
        threadpool.push( [this, th_n, task, c, &MT, n_old]()
            { ((*this).*task)( (c*this->db.size())/th_n, ((c+1)*this->db.size())/th_n, n_old, MT); }
          );
    }
    threadpool.wait_work();
    invalidate_sorting();
    invalidate_histo();
    refresh_db_collision_checks();
}

void Siever::lift_and_replace_best_lift(ZT * const x_full, unsigned int const i)
{
    assert(i >= params.lift_left_bound);

    if (params.lift_unitary_only)
    {
      bool unitary = false;
      for (unsigned int ii = l; ii < r; ++ii)
      {
        unitary |= std::abs(x_full[ii]) == 1;
      }
      if (!unitary) return;
    }

    FT len_precise = 0.;
    for(unsigned int j = i; j < r; ++j)
    {
      FT yi = std::inner_product(x_full + j, x_full + r, full_muT[j].cbegin()+j, static_cast<FT>(0.));
      len_precise += yi * yi * full_rr[j];
    }
    if (len_precise >= lift_bounds[i]) return; // for loop over i

    std::lock_guard<std::mutex> lock_best_lifts(global_best_lift_so_far_mutex);

    // Make sure the condition still holds after grabbing the mutex
    if (len_precise < lift_bounds[i])
    {
        best_lifts_so_far[i].len = len_precise;
        best_lifts_so_far[i].x.resize(r);
        std::copy(x_full, x_full+r, &(best_lifts_so_far[i].x[0]));
    }
    set_lift_bounds();

}

void Siever::set_lift_bounds()
{
    assert(params.lift_left_bound <= l);
    lift_bounds.resize(l+1);
    FT max_so_far = 0.;
    for (size_t i = params.lift_left_bound; i <= l; ++i)
    {
        FT const bli_len = best_lifts_so_far[i].len;
        if (bli_len == 0.)
        {
            lift_bounds[i] = full_rr[i];
        }
        else
        {
            lift_bounds[i] = std::min(bli_len, full_rr[i]);
        }
        max_so_far = std::max(max_so_far, lift_bounds[i] );
    }
    lift_max_bound = max_so_far;
}




// Babai Lift and return the best vector for insertion at each index i in [0 ... r-1]
// (relatively to the full basis). the outputted vector will be expressed in the full gso basis.

void Siever::best_lifts(long* vecs, double* lens)
{
    std::fill(vecs, &vecs[(l+1) * r], 0);
    std::fill(lens, &lens[l+1], 0.);
    if (!params.otf_lift)
    {
        for (CompressedEntry& ce : cdb)
        {
            lift_and_compare(db[ce.i]);
        }
    }
    for (size_t i = 0; i < l+1; ++i)
    {
        if (best_lifts_so_far[i].len==0.) continue;
        // if (best_lifts_so_far[i].len > delta * full_rr[i]) continue;
        for (size_t j = 0; j < r; ++j)
        {
            vecs[i * r + j] = best_lifts_so_far[i].x[j];
        }
        lens[i] = best_lifts_so_far[i].len;
    }
}

// sorts cdb and only keeps the best N vectors.
void Siever::shrink_db(unsigned long N)
{
    CPUCOUNT(207);
    switch_mode_to(SieveStatus::plain);
    assert(N <= cdb.size());

    if (N == 0)
    {
        cdb.clear();
        db.clear();
        invalidate_sorting();
        invalidate_histo();
        return;
    }

    parallel_sort_cdb();

    std::vector<IT> db_to_cdb(db.size());
    for (size_t i = 0; i < cdb.size(); ++i)
        db_to_cdb[cdb[i].i] = i;
    // reorder db in place according to cdb
    // backwards! only down to position N. Note N > 0.
    for (size_t i = db.size()-1; i >= N; --i)
    {
        if (cdb[i].i == i)
            continue;
        // db[j] should be at db[i]
        size_t j = cdb[i].i;
        // swap db[i] and db[j]
        std::swap(db[i], db[j]);
        std::swap(db_to_cdb[i], db_to_cdb[j]);
        // update both cdb[].i
        cdb[db_to_cdb[i]].i = i;
        cdb[db_to_cdb[j]].i = j;
    }
    for (size_t i = N; i < cdb.size(); ++i)
        uid_hash_table.erase_uid(db[cdb[i].i].uid);
    cdb.resize(N);
    db.resize(N);
    status_data.plain_data.sorted_until = N;
    invalidate_histo();
}


// Load an external database of size N -- untested! Might not work.

// TODO: Recompute data inside this function
void Siever::load_db(unsigned int N, long const* db_)
{
    std::array<ZT,MAX_SIEVING_DIM> x;
    std::fill(x.begin(),x.end(),0);
    for (size_t i = 0; i < N; ++i)
    {
        for (size_t j = 0; j < n; ++j)
        {
            x[j] = db_[i*n + j];
        }
        insert_in_db_and_uid(x); // this function is only used from here at the moment.
    }
    switch_mode_to(SieveStatus::plain);
    invalidate_sorting();
    invalidate_histo();
}

// Save to an external database (select the N shortest ones) -- untested, might not work.
void Siever::save_db(unsigned int N, long* db_)
{
    switch_mode_to(SieveStatus::plain);
    parallel_sort_cdb();
    assert(N <= cdb.size());

    for (size_t i = 0; i < N; ++i)
    {
        for (size_t j = 0; j < n; ++j)
        {
            db_[i*n + j] = db[cdb[i].i].x[j];
        }
    }
}

// sorts the current cdb. We keep track of how far the database is already sorted to avoid resorting
// and to avoid screwing with the gauss sieves (for Gauss sieves cdb is split into a list and a queue which are
// separately sorted)
void Siever::parallel_sort_cdb()
{
    CPUCOUNT(209);
    static_assert(static_cast<int>(SieveStatus::LAST) == 4, "Need to update this function");
    if(sieve_status == SieveStatus::plain || sieve_status == SieveStatus::bgj1)
    {
        StatusData::Plain_Data &data = status_data.plain_data;
        assert(data.sorted_until <= cdb.size());
        assert(std::is_sorted(cdb.cbegin(), cdb.cbegin() + data.sorted_until, &compare_CE  ));
        if(data.sorted_until == cdb.size())
        {
            return; // nothing to do. We do not increase the statistics counter.
        }

        // size of the unsorted part of cdb.
        size_t const size_left = cdb.size() - data.sorted_until;
        auto const start_unsorted = cdb.begin() + data.sorted_until;

        // number of threads we wish to have.
        size_t const th_n = std::min( params.threads, static_cast<size_t>(1 + size_left / (10 * MIN_ENTRY_PER_THREAD)));

        for (size_t c = 0; c < th_n; ++c)
        {
            size_t N0 = (size_left * c) / th_n;
            size_t N1 = (size_left * (c+1)) / th_n;
            assert( (c < th_n) || (N1 == size_left) );
            if (c+1 < th_n) std::nth_element(start_unsorted+N0, start_unsorted+N1, cdb.end(), &compare_CE);
            threadpool.push([N0,N1,start_unsorted](){ std::sort(start_unsorted+N0, start_unsorted+N1, &compare_CE) ;});
        }
        threadpool.wait_work();
        std::inplace_merge(cdb.begin(), start_unsorted, cdb.end(), &compare_CE);
        data.sorted_until = cdb.size();
        assert(std::is_sorted(cdb.cbegin(), cdb.cend(), &compare_CE ));
        // TODO: statistics.inc_stats_sorting_overhead();
        return;
    }
    else if(sieve_status == SieveStatus::gauss || sieve_status == SieveStatus::triple_mt)
    {
        StatusData::Gauss_Data &data = status_data.gauss_data;
        assert(data.list_sorted_until <= data.queue_start);
        assert(data.queue_start <= data.queue_sorted_until);
        assert(data.queue_sorted_until <= cdb.size());
        assert(std::is_sorted(cdb.cbegin(), cdb.cbegin()+ data.list_sorted_until, &compare_CE )  );
        assert(std::is_sorted(cdb.cbegin()+ data.queue_start, cdb.cbegin() + data.queue_sorted_until, &compare_CE ));
        size_t const unsorted_list_left = data.queue_start - data.list_sorted_until;
        size_t const unsorted_queue_left = cdb.size() - data.queue_sorted_until;
        if ( (unsorted_list_left == 0) && (unsorted_queue_left == 0))
        {
            return; // nothing to do.
        }
        assert(unsorted_list_left + unsorted_queue_left > 0);
        size_t max_threads_list  = (params.threads * unsorted_list_left + unsorted_list_left + unsorted_queue_left - 1) / (unsorted_list_left + unsorted_queue_left);
        size_t max_threads_queue = params.threads - max_threads_list;
        if (unsorted_list_left > 0 && max_threads_list == 0)
            max_threads_list = 1;
        if (unsorted_queue_left > 0 && max_threads_queue == 0)
            max_threads_queue = 1;

        if(unsorted_list_left > 0)
        {
            auto const start_unsorted = cdb.begin() + data.list_sorted_until;
            auto const end_unsorted   = cdb.begin() + data.queue_start;
            size_t const th_n_list = std::min(max_threads_list, static_cast<size_t>(1+ unsorted_list_left / (10* MIN_ENTRY_PER_THREAD)));
            for(size_t c = 0; c < th_n_list; ++c)
            {
                size_t const N0 = (unsorted_list_left * c) / th_n_list;
                size_t const N1 = (unsorted_list_left * (c+1)) / th_n_list;
                assert( (c < th_n_list) || (N1 == unsorted_list_left) );
                if( c + 1 < th_n_list) std::nth_element(start_unsorted + N0, start_unsorted + N1, end_unsorted, &compare_CE);
                threadpool.push([N0,N1,start_unsorted](){ std::sort(start_unsorted+N0, start_unsorted+N1, &compare_CE) ;});
            }
            if(params.threads == 1 && unsorted_queue_left > 0)
                threadpool.wait_work();
        }
        if(unsorted_queue_left > 0)
        {
            auto const start_unsorted = cdb.begin() + data.queue_sorted_until; // start of range to sort
            auto const end_unsorted   = cdb.end(); // end of range to sort
            size_t const th_n_queue = std::min(max_threads_queue, static_cast<size_t>(1 + unsorted_queue_left / (10 * MIN_ENTRY_PER_THREAD )));
            for(size_t c = 0; c < th_n_queue; ++c)
            {
                size_t const N0 = (unsorted_queue_left * c) / th_n_queue;
                size_t const N1 = (unsorted_queue_left  * (c+1)) / th_n_queue;
                assert( (c < th_n_queue) || (N1 == unsorted_queue_left) );
                if ( c + 1 < th_n_queue) std::nth_element(start_unsorted + N0, start_unsorted + N1, end_unsorted, &compare_CE);
                threadpool.push([N0, N1, start_unsorted]() {std::sort(start_unsorted+N0, start_unsorted+N1, &compare_CE); } );
            }
        }
        threadpool.wait_work();
        if(data.list_sorted_until > 0)
        {
            threadpool.push([this, &data]{std::inplace_merge(cdb.begin(), cdb.begin() + data.list_sorted_until, cdb.begin() + data.queue_start, &compare_CE); }  );
            if(params.threads == 1)
                threadpool.wait_work();
        }
        if(data.queue_sorted_until >  data.queue_start)
        {
            threadpool.push([this, &data]{std::inplace_merge(cdb.begin()+ data.queue_start, cdb.begin() + data.queue_sorted_until, cdb.end(), &compare_CE);}  );
        }
        threadpool.wait_work();
        assert(std::is_sorted(cdb.cbegin(), cdb.cbegin() + data.queue_start, &compare_CE  ));
        assert(std::is_sorted(cdb.cbegin()+ data.queue_start, cdb.cend(), &compare_CE  ));
        data.list_sorted_until = data.queue_start;
        data.queue_sorted_until = cdb.size();
        return;
    }
    else assert(false);
}

void Siever::grow_db_task(unsigned long Nt, unsigned int large, std::vector<Entry> &ve)
{
    ve.clear();
    ve.reserve(Nt);
    for (size_t i = 0; i < Nt; ++i)
    {
        ve.push_back(sample(large));
        if (!uid_hash_table.insert_uid(ve.back().uid))
            ve.pop_back();
    }
}

void Siever::grow_db(unsigned long N, unsigned int large)
{
    CPUCOUNT(206);

    assert(N >= cdb.size());
    unsigned long const Nt = N - cdb.size();
    reserve(N);

    size_t const th_n = std::min(params.threads, static_cast<size_t>(1 + cdb.size() / (10 * MIN_ENTRY_PER_THREAD)));

    std::vector<std::vector<Entry>> vve;
    vve.resize(th_n);
    // vve[c] is the database of Entries to be added to the database that was constructed by thread #c.
    // We first let every thread populate vve[c] with such vectors and already add their uid.
    // We then merge all vve[c]'s into db in a non-threaded fashion.
    // perform a bounded number of iterations (counted by it), where in each iteration, we try to actually sample enough vectors to reach the
    // desired size if no collisions occured. This way, we can adjust the sampler when the it counter gets too large, which indicates too many collisions.
    for (int it = 0; it < 12; ++it)
    {
        for (size_t batch = 0; batch < Nt; batch += 100*th_n)
        {
            for (size_t c = 0; c < th_n; ++c)
            {
                threadpool.push([this,c,large, &vve](){this->grow_db_task(100, large, vve[c]);});
            }
            threadpool.wait_work();
            for (size_t c = 0; c < th_n; ++c)
            {
                for (auto& v : vve[c])
                {
                    if (cdb.size() < N)
                        insert_in_db(std::move(v));
                    else
                        uid_hash_table.erase_uid(v.uid);
                }
            }
            if (cdb.size() >= N)
                return;
        }
        if (it > 3) large++;
    }
    // did not reach size N after 12 iterations: We log a warning and continue with a smaller db size than requested.
    {
        std::cerr << "[sieving.cpp] Warning : All new sample collide. Oversaturated ?" << std::endl;
        std::cerr << n << " " << cdb.size() << "/" << N << std::endl;
    }
    // Note :   Validity of histo is unaffected.
    //          Validity of sorting is also unaffected(!) in the sense that sorted_until's remain valid.
}


void Siever::db_stats(double* min_av_max, long* cumul_histo)
{
    parallel_sort_cdb();
    min_av_max[0] = cdb[0].len;
    min_av_max[1] = 0;
    min_av_max[2] = 0;

    recompute_histo();

    for (size_t i = 0; i < size_of_histo; ++i)
    {
        cumul_histo[i] = histo[i];
        if (i) cumul_histo[i] += cumul_histo[i-1];
    }
}
