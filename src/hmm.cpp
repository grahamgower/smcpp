#include "hmm.h"

long num_blocks(int total_loci, int block_size, int alt_block_size, int mask_freq, int mask_offset)
{
    long denom = (mask_freq - 1) * block_size + alt_block_size;
    long base = total_loci / denom;
    base *= mask_freq;
    long remain = total_loci % denom;
    bool first = true;
    while (remain > 0)
    {
        if (first)
        {
            first = false;
            remain -= alt_block_size;
        }
        else
            remain -= block_size;
        base++;
    }
    return base;
}

Matrix<int> make_two_mask(int n, int m)
{
    Matrix<int> two_mask(n, m);
    two_mask.fill(0);
    two_mask.row(1).fill(1);
    return two_mask;
}

HMM::HMM(const Matrix<int> &obs, int n, const int block_size,
        const Vector<adouble> *pi, const Matrix<adouble> *transition, 
        const Matrix<adouble> *emission, const Matrix<int> emission_mask,
        const int mask_freq, const int mask_offset) : 
    n(n), block_size(block_size), alt_block_size(1),
    pi(pi), transition(transition), emission(emission), emission_mask(emission_mask),
    two_mask(make_two_mask(3, emission_mask.cols())),
    mask_freq(mask_freq), mask_offset(mask_offset),
    M(pi->rows()), 
    Ltot(num_blocks(obs.col(0).sum(), block_size, alt_block_size, mask_freq, mask_offset)),
    Bptr(Ltot), logBptr(Ltot), dBptr(Ltot),
    B(1, 1), 
    alpha_hat(M, Ltot), beta_hat(M, Ltot), gamma(M, Ltot), xisum(M, M), c(Ltot) 
{ 
    prepare_B(obs);
}

bool HMM::is_alt_block(int block) 
{
    return (block + mask_offset) % mask_freq == 0;
}

inline mpz_class multinomial(const std::vector<int> &ks)
{
    mpz_class num, den, tmp;
    int sum = 0;
    den = 1_mpz;
    for (auto k : ks) 
    {
        sum += k;
        mpz_fac_ui(tmp.get_mpz_t(), k);
        den *= tmp;
    }
    mpz_fac_ui(num.get_mpz_t(), sum);
    return num / den;
}

void HMM::prepare_B(const Matrix<int> &obs)
{
    PROGRESS("preparing B");
    std::map<std::pair<int, int>, int> powers;
    Vector<adouble> tmp(M);
    const Matrix<adouble> *em_ptr;
    bool alt_block;
    unsigned long int R, i = 0, block = 0, tobs = 0;
    block_key key;
    unsigned long int L = obs.col(0).sum();
    std::map<Eigen::Array<adouble, Eigen::Dynamic, 1>*, std::vector<int> > block_map;
    int current_block_size = is_alt_block(0) ? alt_block_size : block_size;
    std::pair<int, int> ob;
    mpz_class coef;
    for (unsigned long int ell = 0; ell < obs.rows(); ++ell)
    {
        R = obs(ell, 0);
        ob = {obs(ell, 1), obs(ell, 2)};
        for (int r = 0; r < R; ++r)
        {
            alt_block = is_alt_block(block);
            powers[ob]++;
            if (tobs > L)
                throw std::domain_error("what?");
            tobs++;
            if (++i == current_block_size or (r == R - 1 and ell == obs.rows() - 1))
            {
                i = 0;
                key.alt_block = alt_block;
                key.powers = powers;
                if (block_prob_map.count(key) == 0)
                {
                    tmp.setOnes();
                    block_prob_map[key] = std::make_tuple(tmp, tmp.array().log(), tmp.template cast<double>());
                    block_prob_map_keys.push_back(key);
                    std::array<std::map<std::set<int>, int>, 4> classes;
                    std::vector<int> ctot(4, 0);
                    const Matrix<int> &emask = alt_block ? emission_mask : two_mask;
                    int ai;
                    for (auto &p : powers)
                    {
                        std::set<int> s;
                        int a = p.first.first, b = p.first.second;
                        if (a >= 0 and b >= 0)
                        {
                            ai = 0;
                            s = {emask(a, b)};
                        }
                        else if (a >= 0)
                        {
                            for (int bb = 0; bb < n + 1; ++bb)
                                s.insert(emask(a, bb));
                            ai = 1;
                        }
                        else if (b >= 0)
                        {
                            // a is missing => sum along cols
                            s = {emask(0, b), emask(1, b), emask(2, b)};
                            ai = 2;
                        }
                        else
                        {
                           ai = 3;
                        }
                        if (classes[ai].count(s) == 0)
                            classes[ai].emplace(s, 0);
                        classes[ai][s] += p.second;
                        ctot[ai] += p.second;
                    }
                    coef = multinomial(ctot);
                    for (int j = 0; j < 4; ++j)
                    {
                        std::vector<int> values;
                        for (auto &kv : classes[j])
                            values.push_back(kv.second);
                        coef *= multinomial(values);
                    }
                    comb_coeffs[key] = coef.get_ui();
                }
                Bptr[block] = &std::get<0>(block_prob_map[key]);
                logBptr[block] = &std::get<1>(block_prob_map[key]);
                dBptr[block] = &std::get<2>(block_prob_map[key]);
                block_map[logBptr[block]].push_back(block);
                current_block_size = is_alt_block(block + 1) ? alt_block_size : block_size;
                block_keys.push_back({key.alt_block, key.powers});
                block++;
                powers.clear();
            }
        }
    }
    for (auto &p : block_map) block_pairs.push_back(p);
    // for (auto &p : block_prob_map)
        // reverse_map[&block_prob_map[p.first].second] = p.first;
    PROGRESS_DONE();
}

double HMM::loglik()
{
    double ret = c.array().log().sum();
    return ret;
}

void HMM::domain_error(double ret)
{
    if (std::isinf(ret) or std::isnan(ret))
    {
        std::cout << pi->template cast<double>() << std::endl << std::endl;
        std::cout << transition->template cast<double>() << std::endl << std::endl;
        std::cout << emission->template cast<double>() << std::endl << std::endl;
        throw std::domain_error("badness encountered");
    }
}

/*
template <typename T>
void HMM<T>::viterbi(void)
{
    Eigen::MatrixXd log_transition = transition.template cast<double>().array().log();
    Eigen::MatrixXd log_emission = emission.template cast<double>().array().log();
    std::vector<double> V(M), V1(M);
    std::vector<std::vector<int>> path(M), newpath(M);
    Eigen::MatrixXd pid = pi.template cast<double>();
    std::vector<int> zeros(M, 0);
    double p, p2, lemit;
    int st;
    for (int m = 0; m < M; ++m)
    {
        V[m] = log(pid(m)) + log_emission(m, emission_index(n, obs(0,1), obs(0,2)));
        path[m] = zeros;
        path[m][m]++;
    }
    for (int ell = 0; ell < L; ++ell)
    {
        int R = obs(ell, 0);
        if (ell == 0)
            R--;
        for (int r = 0; r < R; ++r)
        {
            for (int m = 0; m < M; ++m)
            {
                lemit = log_emission(m, emission_index(n, obs(ell, 1), obs(ell, 2)));
                p = -INFINITY;
                st = 0;
                for (int m2 = 0; m2 < M; ++m2)
                {
                    p2 = V[m2] + log_transition(m2, m) + lemit;
                    if (p2 > p)
                    {
                        p = p2;
                        st = m2;
                    }
                }
                V1[m] = p;
                newpath[m] = path[st];
                newpath[m][m]++;
            }
            path = newpath;
            V = V1;
        }
    }
    p = -INFINITY;
    st = 0;
    for (int m = 0; m < M; ++m)
    {
        if (V[m] > p)
        {
            p = V[m];
            st = m;
        }
    }
    viterbi_path = path[st];
    return viterbi_path;
}
*/

#include "gsl/gsl_sf_gamma.h"
#include "gsl/gsl_sf_psi.h"

template <typename T>
T gamma_function(const T&);

template <>
double gamma_function(const double &x) { return gsl_sf_gamma(x); }

template <>
adouble gamma_function(const adouble &x)
{
    adouble ret(0., x.derivatives());
    ret.value() = gamma_function(x.value());
    ret.derivatives() = ret.value() * gsl_sf_psi_n(0, x.value()) * x.derivatives();
    return ret;
}

template <typename Der>
typename Eigen::DenseBase<Der>::Scalar dirichlet_multinomial_C(const Eigen::DenseBase<Der> &alpha, const std::map<int, int> &counts)
{
    typename Eigen::DenseBase<Der>::Scalar ret(1.0), sm(0.0), sm_alpha(0.0), alpha_i;
    const double alpha0 = 0.05;
    for (int i = 0; i < alpha.cols(); ++i)
    {
        alpha_i = alpha(0, i);
        alpha_i *= alpha0;
        sm_alpha += alpha_i;
        if (counts.count(i))
        {
            ret /= gamma_function(alpha_i);
            alpha_i += counts.at(i);
            ret *= gamma_function(alpha_i);
        }
        sm += alpha_i;
    }
    ret *= gamma_function(sm_alpha) / gamma_function(sm);
    return ret;
}

void HMM::recompute_B(void)
{
    PROGRESS("recompute B");
    std::map<int, Vector<adouble> > mask_probs, two_probs;
    int em;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < n + 1; ++j)
        {
            em = emission_mask(i, j);
            if (mask_probs.count(em) == 0)
                mask_probs[em] = Vector<adouble>::Zero(M);
            mask_probs[em] += emission->col((n + 1) * i + j);
            if (two_probs.count(i % 2) == 0)
                two_probs[i % 2] = Vector<adouble>::Zero(M);
            two_probs[i % 2] += emission->col((n + 1) * i + j);
        }
#pragma omp parallel for
    for (auto it = block_prob_map_keys.begin(); it < block_prob_map_keys.end(); ++it)
    {
        Eigen::Array<adouble, Eigen::Dynamic, 1> tmp = Eigen::Array<adouble, Eigen::Dynamic, 1>::Ones(M), tmp2;
        Eigen::Array<adouble, Eigen::Dynamic, 1> log_tmp = Eigen::Array<adouble, Eigen::Dynamic, 1>::Zero(M);
        const Matrix<int> &emask = it->alt_block ? emission_mask : two_mask;
        std::map<int, Vector<adouble> > &prbs = it->alt_block ? mask_probs : two_probs;
        Vector<adouble> ob;
        for (auto &p : it->powers)
        {
            ob = Vector<adouble>::Zero(M);
            int a = p.first.first, b = p.first.second;
            if (a == -1)
            {
                if (b == -1)
                    // Double missing!
                    continue;
                else
                {
                    for (int x : std::set<int>{emask(0, b), emask(1, b), emask(2, b)})
                        ob += prbs[x];
                }
            }
            else
            {
                if (b == -1)
                {
                    std::set<int> bbs;
                    for (int bb = 0; bb < emask.cols(); ++bb)
                        bbs.insert(emask(a, bb));
                    for (int x : bbs)
                        ob += prbs[x];
                }
                else
                    ob = prbs[emask(a, b)];
            }
            log_tmp += ob.array().log() * p.second;
        }
        log_tmp += log(comb_coeffs[*it]);
        tmp = exp(log_tmp);
        if (tmp.maxCoeff() > 1.0 or tmp.minCoeff() < 0.0)
            throw std::runtime_error("probability vector not in [0, 1]");
        check_nan(tmp);
        check_nan(log_tmp);
        std::get<0>(block_prob_map[*it]) = tmp.matrix();
        std::get<1>(block_prob_map[*it]) = log_tmp;
        std::get<2>(block_prob_map[*it]) = tmp.matrix().template cast<double>();
    }
    PROGRESS_DONE();
}

void HMM::forward_backward(void)
{
    PROGRESS("forward backward");
    Matrix<double> tt = transition->template cast<double>();
    Matrix<double> ttpow = tt.pow(block_size);
    Matrix<double> ttalt = tt.pow(alt_block_size);
    alpha_hat.col(0) = pi->template cast<double>().cwiseProduct(Bptr[0]->template cast<double>());
	c(0) = alpha_hat.col(0).sum();
    alpha_hat.col(0) /= c(0);
    for (int ell = 1; ell < Ltot; ++ell)
    {
        alpha_hat.col(ell) = Bptr[ell]->template cast<double>().asDiagonal() * 
            (is_alt_block(ell - 1) ? ttalt : ttpow).transpose() * alpha_hat.col(ell - 1);
        c(ell) = alpha_hat.col(ell).sum();
        if (std::isnan(toDouble(c(ell))))
            throw std::domain_error("something went wrong in forward algorithm");
        alpha_hat.col(ell) /= c(ell);
    }
    beta_hat.col(Ltot - 1) = Vector<double>::Ones(M);
    for (int ell = Ltot - 2; ell >= 0; --ell)
        beta_hat.col(ell) = (is_alt_block(ell) ? ttalt : ttpow) * 
            Bptr[ell + 1]->template cast<double>().asDiagonal() * beta_hat.col(ell + 1) / c(ell + 1);
    PROGRESS_DONE();
}


void HMM::Estep(void)
{
    PROGRESS("E step");
    forward_backward();
	gamma = alpha_hat.cwiseProduct(beta_hat);
    PROGRESS("xisum");
    Matrix<double> xis = Matrix<double>::Zero(M, M);
    Matrix<double> xis_alt = Matrix<double>::Zero(M, M);
#pragma omp parallel
    {
        Matrix<double> tmp(M, M), xis_p = Matrix<double>::Zero(M, M), xis_alt_p = Matrix<double>::Zero(M, M);
#pragma omp for nowait schedule(static)
        for (int ell = 1; ell < Ltot; ++ell)
        {
            tmp.noalias() = alpha_hat.col(ell - 1) * beta_hat.col(ell).transpose() * dBptr[ell]->asDiagonal() / c(ell);
            if (is_alt_block(ell - 1))
                xis_alt_p += tmp;
            else
                xis_p += tmp;
        }
#pragma omp critical
        {
            xis_alt += xis_alt_p;
            xis += xis_p;
        }
    }
    PROGRESS("xisum done");
    Matrix<double> tr = transition->template cast<double>().pow(block_size);
    Matrix<double> tralt = transition->template cast<double>().pow(alt_block_size);
    xisum = xis.cwiseProduct(tr);
    xisum_alt = xis_alt.cwiseProduct(tralt);
    PROGRESS_DONE();
}

Matrix<adouble> mymatpow(const Matrix<adouble> M, int p)
{
    if (p == 1)
        return M;
    if (p % 2 == 0) 
    {
        Matrix<adouble> M2 = mymatpow(M, p / 2);
        return M2 * M2;
    }
    Matrix<adouble> M2 = mymatpow(M, (p - 1) / 2);
    return M * M2 * M2;
}

adouble HMM::Q(void)
{
    PROGRESS("HMM::Q");
    Eigen::Array<adouble, Eigen::Dynamic, Eigen::Dynamic> xis = xisum.template cast<adouble>().array();
    Eigen::Array<adouble, Eigen::Dynamic, Eigen::Dynamic> xis_alt = xisum_alt.template cast<adouble>().array();
    adouble ret1, ret2, ret3;
    ret1 = (gamma.col(0).array().template cast<adouble>() * pi->array().log()).sum();
    std::map<const decltype(logBptr)::value_type, Eigen::Array<adouble, Eigen::Dynamic, 1> > counts;
    ret2 = 0.0;
#pragma omp parallel
    {
        adouble sum_private(0.0);
#pragma omp for nowait schedule(static)
        for (auto it = block_pairs.begin(); it < block_pairs.end(); ++it)
        {
            Vector<double> gamma_sum(M);
            gamma_sum.setZero();
            for (int ell : it->second)
                gamma_sum += gamma.col(ell);
            sum_private += (*(it->first) * gamma_sum.array().template cast<adouble>()).sum();
        }
#pragma omp critical
        {
            ret2 += sum_private;
        }
    }
    Eigen::Array<adouble, Eigen::Dynamic, Eigen::Dynamic> ttpow = mymatpow(*transition, block_size).array().log();
    Eigen::Array<adouble, Eigen::Dynamic, Eigen::Dynamic> ttalt = mymatpow(*transition, alt_block_size).array().log();
    // check_nan(tt);
    check_nan(xis);
    check_nan(xis_alt);
    ret3 = (xis * ttpow).sum();
    ret3 += (xis_alt * ttalt).sum();
    PROGRESS_DONE();
    /*
    std::vector<decltype(counts)::value_type*> a;
    for (auto &p : counts)
        a.push_back(&p);
    std::sort(a.begin(), a.end(), 
            [] (const decltype(counts)::value_type *a, const decltype(counts)::value_type *b)
            { return a->second > b->second; });
    for (auto aa : a)
    {
        if (aa->second < 100)
            continue;
        std::cout << "count: " << aa->second << std::endl;
        std::cout << reverse_map[aa->first] << std::endl;
        std::cout << aa->first->template cast<double>().transpose() << std::endl << std::endl;
    }
    */
    check_nan(ret1);
    check_nan(ret2);
    check_nan(ret3);
    PROGRESS("ret1:" << ret1 << " [" << ret1.derivatives().transpose() << "]\nret2:" 
            << ret2 << " [" << ret2.derivatives().transpose() << "]\nret3:" << ret3 
            << " [" << ret3.derivatives().transpose() << "]\n");
    return ret1 + ret2 + ret3;
}
