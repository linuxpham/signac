#define ARMA_USE_CXX11
#define ARMA_NO_DEBUG

#define HARMONY_RATIO 1e-5
#define HARMONY_EPS 1e-50

#include <RcppArmadillo.h>
#include <RcppParallel.h>

#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <limits>
#include <cmath>
#include <algorithm>
#include <functional>
#include <assert.h>

#include "CommonUtil.h"
#include "incbeta.h"

using namespace Rcpp;
using namespace arma;
using namespace RcppParallel;

// [[Rcpp::depends(RcppParallel)]]
// [[Rcpp::depends(RcppArmadillo)]]
// [[Rcpp::depends(Rhdf5lib)]]

std::vector<double> init_pmf(double r, double p, int k) {
    std::vector<double> prob;

    prob.resize(k);

    prob[0] = std::pow(1 - p,r);
    for (int i = 1; i < k; ++i)
        prob[i] = prob[i-1] * (i + r - 1) / i * p;
    return prob;
}

double harmonic_mean(double a, double b) {
    if (fmin(a,b) < HARMONY_EPS)
        return 0;

    return 2 * a * b / (a + b);
}

bool compare(int i, int j, const std::vector<double> &res) {
    return res[i] < res[j];
}

void read_cluster(const std::string barcode,
                  const std::string cluster,
                  std::vector<std::pair<std::string, int> > &result)
{
    std::unordered_map<std::string, int> m;
    std::ifstream fin;
    fin.open(barcode, std::ios::in);
    while(fin) {
        std::string bar;
        std::getline(fin, bar);

        bar = bar.substr(0, bar.find('-'));
        m[bar] = result.size();
        result.push_back(std::make_pair(bar, -1));
    }

    fin.close();

    fin.open(cluster, std::ios::in);

    fin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    while (fin) {
        std::string bar;
        int clus;

        fin >> bar >> clus;

        auto i = m.find(bar);

        if (i == m.end())
            continue;

        result[i->second].second = clus;
    }

    fin.close();
}

void read_gene(std::string file,
               std::vector<std::string> &result)
{
    std::ifstream fin;

    fin.open(file, std::ios::in);

    while (fin) {
        std::string line;
        std::getline(fin, line);
        result.push_back(line);
    }

    fin.close();
}

void HarmonyFit(const arma::sp_mat &mtx,
                const std::vector<std::pair<std::string, int>> &cluster,
                int cid,
                std::vector<std::pair<double,double> > &res)
{
    res.resize(mtx.n_rows, std::make_pair(0.0,0.0));
    arma::sp_mat::const_col_iterator c_it;

    for (int i = 0; i < mtx.n_cols; ++i) {
        for (c_it = mtx.begin_col(i); c_it != mtx.end_col(i); ++c_it) {
            if (cluster[i].second != cid)
                continue;

            res[c_it.row()].first += (*c_it);
        }
    }

    int total = 0;
    for (int i = 0 ; i < mtx.n_cols; ++i)
        total += (cluster[i].second == cid);

    for (int i = 0; i < mtx.n_rows; ++i)
        res[i].first /= total;

    std::vector<int> zero;
    zero.resize(mtx.n_rows, total);

    for (int i = 0; i < mtx.n_cols; ++i) {
        for (c_it = mtx.begin_col(i); c_it != mtx.end_col(i); ++c_it) {
            if (cluster[i].second != cid)
                continue;
            res[c_it.row()].second += std::pow((*c_it) - res[c_it.row()].first, 2);
            --zero[c_it.row()];
        }
    }

    for (int i = 0; i < mtx.n_rows; ++i) {
        res[i].second += zero[i] * std::pow(res[i].first,2);
        res[i].second /= total - 1;

        if (res[i].second < res[i].first * (1 + HARMONY_RATIO))
            res[i].second = res[i].first * (1 + HARMONY_RATIO);
    }
}

void HarmonyCheck(
        const arma::sp_mat &mtx,
        const std::vector<std::pair<std::string, int>> &cluster,
        int cid,
        const std::vector<std::pair<double, double>> &dist,
        std::vector<std::tuple<double, double, double, double> > &res)
{
    std::vector<std::vector<std::pair<int,int>>> count;
    count.resize(mtx.n_rows, std::vector<std::pair<int,int>>());
    res.resize(mtx.n_rows, std::make_tuple(0.0, 0.0, 0.0, 1.0));
    arma::sp_mat::const_col_iterator c_it;

    for(int i = 0; i < mtx.n_cols; ++i) {
        for (c_it = mtx.begin_col(i); c_it != mtx.end_col(i); ++c_it) {
            int cnt = (*c_it);

            std::vector<std::pair<int, int>> &r_cnt = count[c_it.row()];

            if (cnt >= r_cnt.size())
                r_cnt.resize(cnt + 1);

            if (cluster[i].second == cid)
                ++r_cnt[cnt].first;
            else
                ++r_cnt[cnt].second;
        }
    }

    int total_in = 0;

    for (int i = 0; i < mtx.n_cols; ++i)
        if (cluster[i].second == cid)
            ++total_in;

        int total_out = mtx.n_cols - total_in;
        for (int i = 0; i < mtx.n_rows; ++i) {
            int zero_in = total_in;
            int zero_out = total_out;

            for(int j = 1; j < count[i].size(); ++j) {
                zero_in -= count[i][j].first;
                zero_out -= count[i][j].second;
            }

            if (count[i].size() < 1)
                count[i].resize(1);

            count[i][0] = std::make_pair(zero_in, zero_out);
        }

        double thres_prob_in = std::max(std::min<double>(total_in,5.0),std::pow(total_in,0.6))/total_in;
        double thres_prob_out = std::max(std::min<double>(total_out,5.0),std::pow(total_out,0.6))/total_out;
        double thres_common = std::max(thres_prob_in, thres_prob_out);

        for (int i = 0; i < mtx.n_rows; ++i) {
            double p = 1 - dist[i].first/dist[i].second;
            double r = (1 / p - 1) * dist[i].first;

            if (std::isnan(p) || p <= 0 || p > 1 || r <= 0) {
                p = 0;
                r = 0;
            }

            std::vector<double> prob = init_pmf(r, p, count[i].size() + 1);

            double dprob_in = 0;
            double dprob_out = 0;
            double dprob_both = 0;

            double prob_in = 0;
            double prob_out = 0;

            double prob_in_in = 0;
            double prob_out_in = 0;

            int bin_cnt = 0; //bin_count-1 actually

            for(int j = 0; j < count[i].size(); ++j) {
                prob_in += (double)count[i][j].first/total_in;
                prob_out += (double)count[i][j].second/total_out;

                prob_in_in += (double)count[i][j].first/total_in;
                prob_out_in += (double)count[i][j].second/total_out;

                dprob_in += prob[j];
                dprob_out += prob[j];
                dprob_both += prob[j];

                if (dprob_in >= thres_prob_in) {
                    std::get<0>(res[i]) += harmonic_mean(prob_in, dprob_in);
                    prob_in = dprob_in = 0;
                }

                if (dprob_out >= thres_prob_out) {
                    std::get<1>(res[i]) +=  harmonic_mean(prob_out, dprob_out);
                    prob_out = dprob_out = 0;
                }

                if (dprob_both > thres_common) {
                    std::get<2>(res[i]) += harmonic_mean(prob_in_in, prob_out_in);
                    prob_in_in = prob_out_in = dprob_both = 0;
                    ++bin_cnt;
                }

                //res[i].first += count[i][j].first * (prob[j]);
                //res[i].second += count[i][j].second * (prob[j]);
            }
            std::get<0>(res[i]) += harmonic_mean(prob_in, dprob_in);
            std::get<1>(res[i]) += harmonic_mean(prob_out, dprob_out);
            std::get<2>(res[i]) += harmonic_mean(prob_in_in, prob_out_in);

            double mean = 0.25 * bin_cnt * (1.0/total_in + 1.0/total_out);
            assert(mean >= 0);
            double var = 0.125 * bin_cnt * pow(1.0/total_in + 1.0/total_out,2);
            double scale =  (mean * (1 - mean) / var - 1);
            double shape1 = (1 - mean) * scale;
            double shape2 = mean * scale;

            std::get<3>(res[i]) = incbeta(shape1, shape2, std::get<2>(res[i]));

            //res[i].first += zero_in * prob[0];
            //res[i].second += zero_out * prob[0];
        }

}

// [[Rcpp::export]]
int Harmony(const arma::sp_mat &mtx,
            const std::string matrix_dir,
            const std::string cluster_f,
            const int cid,
            const int ordering,
            const std::string out_f)
{

    std::vector<std::pair<std::string, int>> cluster;
    read_cluster(matrix_dir + "/barcodes.tsv", cluster_f, cluster);
    std::cout << "Done read cluster" << std::endl;

    std::vector<std::pair<double, double>> f;
    HarmonyFit(mtx, cluster, cid, f);
    std::cout << "Done fit distribution" << std::endl;

    std::vector<std::tuple<double, double, double, double>> res;
    HarmonyCheck(mtx, cluster, cid, f, res);
    std::cout << "Done calculate" << std::endl;

    std::vector<std::string> genes;
    read_gene(matrix_dir + "/genes.tsv", genes);

    std::vector<int> order, porder;
    std::vector<double> rate;
    std::vector<double> pvalue;

    order.resize(mtx.n_rows);
    porder.resize(mtx.n_rows);
    rate.resize(mtx.n_rows);
    pvalue.resize(mtx.n_rows);

    for (int i = 0; i < mtx.n_rows; ++i) {
        order[i] = i;
        porder[i] = i;
        pvalue[i] = std::get<3>(res[i]);
        switch(ordering) {
        case 1:
            rate[i] = -sqrt(1 - std::get<0>(res[i])) + sqrt(1-std::get<1>(res[i])); //metric measurement
            if (rate[i] < 0)
                rate[i] = 0;
            rate[i]= 1 - rate[i] * rate[i]; //upper bound on score
            break;
        case 2:
            rate[i] = 1 - std::get<0>(res[i]) + std::get<1>(res[i]); //nonmetric but bounded error
            break;
        case 3:
            rate[i] = std::get<1>(res[i])/std::get<0>(res[i]); //unbounded but very  good estimation :)))) idk :)))
            break;
        case 4:
            rate[i] = std::get<1>(res[i]); //unadjusted :))) good estimation maybe :))))
            break;
        case 5:
            rate[i] = std::get<2>(res[i]); //non parametric and really really awesome xD
            break;
        case 6:
            rate[i] = std::get<3>(res[i]);
            break;
        default:
            std::cout << "unknown ordering method\n" << std::endl;
        return 0;
        }
    }

    std::sort(order.begin(), order.end(), std::bind(compare, std::placeholders::_1, std::placeholders::_2, rate));
    std::sort(porder.begin(), porder.end(), std::bind(compare, std::placeholders::_1, std::placeholders::_2, pvalue));

    std::ofstream fout;
    fout.open(out_f, std::ios::out);

    double prev = 0;
    for(int i = 0; i < mtx.n_rows; ++i) {
        int k = porder[i];
        double p = pvalue[k] * mtx.n_rows / (i + 1);

        if (p > 1)
            p = 1;

        if (p >= prev)
            prev = p;

        pvalue[k] = prev;
    }

    for(int i = 0; i <mtx.n_rows; ++i) {
        int k = order[i];
        fout << genes[k] << " " << rate[k] << " " << f[k].first << " " << f[k].second << " " << std::get<0>(res[k]) << " " << std::get<1>(res[k]) << " " << std::get<2>(res[k]) << " " << std::get<3>(res[k]) << " " << pvalue[k] << std::endl;
    }

    fout.close();

    std::cout << "Done all" << std::endl;

    return 0;
}
