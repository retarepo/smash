/*
 *
 *    Copyright (c) 2016-
 *      SMASH Team
 *
 *    GNU General Public License (GPLv3 or later)
 *
 */

#include <gsl/gsl_sf_bessel.h>

#include <boost/filesystem.hpp>
#include <iostream>
#include <fstream>
#include <iomanip>

#include "include/constants.h"
#include "include/forwarddeclarations.h"
#include "include/hadgas_eos.h"

namespace Smash {

EosTable::EosTable(double de, double dnb, size_t n_e, size_t n_nb) :
  de_(de),
  dnb_(dnb),
  n_e_(n_e),
  n_nb_(n_nb) {
  table_.resize(n_e_*n_nb_);
}

void EosTable::compile_table(HadronGasEos &eos,
                             const std::string& eos_savefile_name) {
  bool table_read_success = false, table_consistency = true;
  if (boost::filesystem::exists(eos_savefile_name)) {
    // Read table from file
    std::cout << "Reading table from file " << eos_savefile_name << std::endl;
    std::ifstream file;
    file.open(eos_savefile_name, std::ios::in);
    file >> de_ >> dnb_;
    file >> n_e_ >> n_nb_;
    table_.resize(n_e_*n_nb_);
    for (size_t ie = 0; ie < n_e_; ie++) {
      for (size_t inb = 0; inb < n_nb_; inb++) {
        double p, T, mub, mus;
        file >> p >> T >> mub >> mus;
        table_[index(ie, inb)] = {p, T, mub, mus};
      }
    }
    table_read_success = true;
    std::cout << "Table consumed successfully." << std::endl;
  }

  if (table_read_success) {
    // Check if the saved table is consistent with the current particle table
    std::cout << "Checking consistency of the table... " << std::endl;
    constexpr size_t number_of_steps = 50;
    const size_t ie_step = 1 + n_e_/number_of_steps;
    const size_t inb_step = 1 + n_nb_/number_of_steps;
    for (size_t ie = 0; ie < n_e_; ie += ie_step) {
      for (size_t inb = 0; inb < n_nb_; inb += inb_step) {
        const table_element x = table_[index(ie, inb)];
        const double e_comp  = eos.energy_density(x.T, x.mub, x.mus);
        const double nb_comp = eos.net_baryon_density(x.T, x.mub, x.mus);
        const double ns_comp = eos.net_strange_density(x.T, x.mub, x.mus);
        const double p_comp  = eos.pressure(x.T, x.mub, x.mus);
        // Precision is just 10^-3, this is precision of saved data in the file
        const double eps = 1.e-3;
        // Only check the physical region, hence T > 0 condition
        if ((std::abs(de_*ie - e_comp) > eps ||
             std::abs(dnb_*inb - nb_comp) > eps ||
             std::abs(ns_comp) > eps ||
             std::abs(x.p - p_comp) > eps) && (x.T > 0.0)) {
           std::cout << "discrepancy: "
             << de_*ie   << " = " << e_comp  << ", "
             << dnb_*inb << " = " << nb_comp << ", "
             << x.p        << " = " << p_comp  << ", "
             << "0"        << " = " << ns_comp << std::endl;
          table_consistency = false;
          goto finish_consistency_check;
        }
      }
    }
  }
  finish_consistency_check:

  if (!table_read_success || !table_consistency) {
    std::cout << "Compiling an EoS table..." << std::endl;
    const double ns = 0.0;
    for (size_t ie = 0; ie < n_e_; ie++) {
      const double e = de_ * ie;
      std::array<double, 3> init_approx = {0.1, 0.0, 0.0};
      for (size_t inb = 0; inb < n_nb_; inb++) {
        const double nb = dnb_ * inb;
        // It is physically impossible to have energy density > nucleon mass*nb,
        // therefore eqns have no solutions.
        if (nb*nucleon_mass >= e) {
          table_[index(ie, inb)] = {0.0, 0.0, 0.0, 0.0};
          continue;
        }
        // Take extrapolated (T, mub, mus) as initial approximation, but not
        // for cases close to unphysical region
        if (nb > e) {
          init_approx = {0.1, 0.7, 0.0};
        } else if (inb >= 2) {
          const table_element y = table_[index(ie, inb - 2)];
          const table_element x = table_[index(ie, inb - 1)];
          init_approx = {2.0*x.T - y.T, 2.0*x.mub - y.mub, 2.0*x.mus - y.mus};
        }
        const std::array<double, 3> res = eos.solve_eos(e, nb, ns, init_approx);
        const double T   = res[0];
        const double mub = res[1];
        const double mus = res[2];
        table_[index(ie, inb)] = {eos.pressure(T, mub, mus), T, mub, mus};
      }
    }
    // Save table to file
    std::cout << "Saving table to file " << eos_savefile_name << std::endl;
    std::ofstream file;
    file.open(eos_savefile_name, std::ios::out);
    file << de_ << " " << dnb_ << std::endl;
    file << n_e_ << " " << n_nb_ << std::endl;
    file << std::setprecision(7);
    file << std::fixed;
    for (size_t ie = 0; ie < n_e_; ie++) {
      for (size_t inb = 0; inb < n_nb_; inb++) {
        const EosTable::table_element x = table_[index(ie, inb)];
        file << x.p << " " <<
                x.T << " " <<
                x.mub << " " <<
                x.mus << std::endl;
      }
    }
  }
}

void EosTable::get(EosTable::table_element& res, double e, double nb) const {
  const size_t ie  = static_cast<size_t>(std::floor(e/de_));
  const size_t inb = static_cast<size_t>(std::floor(nb/dnb_));

  if (ie >= n_e_ - 1 || inb >= n_nb_ - 1) {
    res = {-1.0, -1.0, -1.0, -1.0};
  } else {
    // 1st order interpolation
    const double ae = e/de_ - ie;
    const double an = nb/dnb_ - inb;
    const EosTable::table_element s1  = table_[index(ie,     inb)];
    const EosTable::table_element s2  = table_[index(ie + 1, inb)];
    const EosTable::table_element s3  = table_[index(ie    , inb + 1)];
    const EosTable::table_element s4  = table_[index(ie + 1, inb + 1)];
    res.p = ae*(an*s4.p + (1.0-an)*s2.p) + (1.0-ae)*(an*s3.p + (1.0-an)*s1.p);
    res.T = ae*(an*s4.T + (1.0-an)*s2.T) + (1.0-ae)*(an*s3.T + (1.0-an)*s1.T);
    res.mub = ae*(an*s4.mub + (1.0-an)*s2.mub) +
              (1.0-ae)*(an*s3.mub + (1.0-an)*s1.mub);
    res.mus = ae*(an*s4.mus + (1.0-an)*s2.mus) +
              (1.0-ae)*(an*s3.mus + (1.0-an)*s1.mus);
  }
}

HadronGasEos::HadronGasEos(const bool tabulate) :
  x_(gsl_vector_alloc(n_equations_)),
  tabulate_(tabulate) {
  const gsl_multiroot_fsolver_type *solver_type;
  solver_type = gsl_multiroot_fsolver_dnewton;
  solver_ = gsl_multiroot_fsolver_alloc(solver_type, n_equations_);
  if (tabulate_) {
    eos_table_.compile_table(*this);
  }
}

HadronGasEos::~HadronGasEos() {
  gsl_multiroot_fsolver_free(solver_);
  gsl_vector_free(x_);
}

const ParticleTypePtrList HadronGasEos::list_eos_particles() {
  ParticleTypePtrList list;
  list.reserve(ParticleType::list_all().size());
  for (const ParticleType &ptype : ParticleType::list_all()) {
    if (is_eos_particle(ptype)) {
      list.emplace_back(&ptype);
    }
  }
  return list;
}

double HadronGasEos::scaled_partial_density(const ParticleType& ptype,
                                         double beta, double mub, double mus) {
  const double z = ptype.mass()*beta;
  double x = beta*(ptype.baryon_number()*mub +
                   ptype.strangeness()*mus -
                   ptype.mass());
  const size_t g = ptype.spin() + 1;
  x = (x < -700.0) ? 0.0 : std::exp(x);
  // The case of small mass: K_n(z) -> (n-1)!/2 *(2/z)^n, z -> 0
  // z*z*K_2(z) -> 2
  return (z < really_small) ? 2.0*g*x :
           z*z * g*x * gsl_sf_bessel_Kn_scaled(2, z);
}

double HadronGasEos::partial_density(const ParticleType& ptype,
                                     double T, double mub, double mus) {
  if (T < really_small) {
    return 0.0;
  }
  return prefactor_ * T*T*T * scaled_partial_density(ptype, 1.0/T, mub, mus);
}

double HadronGasEos::energy_density(double T, double mub, double mus) {
  if (T < really_small) {
    return 0.0;
  }
  const double beta = 1.0/T;
  double e = 0.0;
  for (const ParticleType &ptype : ParticleType::list_all()) {
    if (!is_eos_particle(ptype)) {
      continue;
    }
    const double z = ptype.mass()*beta;
    double x = beta * (mub*ptype.baryon_number() +
                       mus*ptype.strangeness() -
                       ptype.mass());
    x = (x < -700.0) ? 0.0 : std::exp(x);
    const size_t g = ptype.spin() + 1;
    // Small mass case, z*z*K_2(z) -> 2, z*z*z*K_1(z) -> 0 at z->0
    e += (z < really_small) ? 3.0*g*x :
           z*z * g*x * (3.0*gsl_sf_bessel_Kn_scaled(2, z) +
                        z * gsl_sf_bessel_K1_scaled(z));
  }
  e *= prefactor_ * T*T*T*T;
  return e;
}

double HadronGasEos::density(double T, double mub, double mus) {
  if (T < really_small) {
    return 0.0;
  }
  const double beta = 1.0/T;
  double rho = 0.0;
  for (const ParticleType &ptype : ParticleType::list_all()) {
    if (!is_eos_particle(ptype)) {
      continue;
    }
    rho += scaled_partial_density(ptype, beta, mub, mus);
  }
  rho *= prefactor_ * T*T*T;
  return rho;
}

double HadronGasEos::net_baryon_density(double T, double mub, double mus) {
  if (T < really_small) {
    return 0.0;
  }
  const double beta = 1.0/T;
  double rho = 0.0;
  for (const ParticleType &ptype : ParticleType::list_all()) {
    if (!ptype.is_baryon() || !is_eos_particle(ptype)) {
      continue;
    }
    rho += scaled_partial_density(ptype, beta, mub, mus) *
           ptype.baryon_number();
  }
  rho *= prefactor_ * T*T*T;
  return rho;
}

double HadronGasEos::net_strange_density(double T, double mub, double mus) {
  if (T < really_small) {
    return 0.0;
  }
  const double beta = 1.0/T;
  double rho = 0.0;
  for (const ParticleType &ptype : ParticleType::list_all()) {
    if (ptype.strangeness() == 0 || !is_eos_particle(ptype)) {
      continue;
    }
    rho += scaled_partial_density(ptype, beta, mub, mus) *
           ptype.strangeness();
  }
  rho *= prefactor_ * T*T*T;
  return rho;
}

double HadronGasEos::mus_net_strangeness0(double T, double mub) {
  // Binary search
  double mus_u = mub + T;
  double mus_l = 0.0;
  double mus, rhos;
  size_t iteration = 0;
  // 30 iterations should give precision 2^-30 ~ 10^-9
  const size_t max_iteration = 30;
  do {
    mus = 0.5 * (mus_u + mus_l);
    rhos = net_strange_density(T, mub, mus);
    if (rhos > 0.0) {
      mus_u = mus;
    } else {
      mus_l = mus;
    }
    iteration++;
  } while (std::abs(rhos) > tolerance_ && iteration < max_iteration);
  if (iteration == max_iteration) {
    throw std::runtime_error("Solving rho_s = 0: too many iterations.");
  }
  return mus;
}

int HadronGasEos::set_eos_solver_equations(const gsl_vector* x,
                                    void *params, gsl_vector* f) {
  double e =  reinterpret_cast<struct rparams*>(params)->e;
  double nb = reinterpret_cast<struct rparams*>(params)->nb;
  double ns = reinterpret_cast<struct rparams*>(params)->ns;

  const double T   = gsl_vector_get(x, 0);
  const double mub = gsl_vector_get(x, 1);
  const double mus = gsl_vector_get(x, 2);

  gsl_vector_set(f, 0, energy_density(T, mub, mus) - e);
  gsl_vector_set(f, 1, net_baryon_density(T, mub, mus) - nb);
  gsl_vector_set(f, 2, net_strange_density(T, mub, mus) - ns);

  return GSL_SUCCESS;
}

std::array<double, 3> HadronGasEos::solve_eos(double e, double nb, double ns,
                                 std::array<double, 3> initial_approximation) {
  int status;
  size_t iter = 0;

  struct rparams p = {e, nb, ns};
  gsl_multiroot_function f = {&HadronGasEos::set_eos_solver_equations,
                              n_equations_, &p};

  gsl_vector_set(x_, 0, initial_approximation[0]);
  gsl_vector_set(x_, 1, initial_approximation[1]);
  gsl_vector_set(x_, 2, initial_approximation[2]);

  gsl_multiroot_fsolver_set(solver_, &f, x_);
  do {
    iter++;
    status = gsl_multiroot_fsolver_iterate(solver_);

    // print_solver_state(iter);
    // Avoiding too low temperature
    if (gsl_vector_get(solver_->x, 0) < 0.015) {
      return {0.0, 0.0, 0.0};
    }

    // check if solver is stuck
    if (status) {
        break;
    }
    status = gsl_multiroot_test_residual(solver_->f, tolerance_);
  } while (status == GSL_CONTINUE && iter < 1000);

  if (status != GSL_SUCCESS) {
    print_solver_state(iter);
    throw std::runtime_error(gsl_strerror(status));
  }
  return {gsl_vector_get(solver_->x, 0),
          gsl_vector_get(solver_->x, 1),
          gsl_vector_get(solver_->x, 2)};
}

void HadronGasEos::print_solver_state(size_t iter) const {
  std::cout <<
          "iter = " << iter << "," <<
          " x = "   << gsl_vector_get(solver_->x, 0) << " " <<
                       gsl_vector_get(solver_->x, 1) << " " <<
                       gsl_vector_get(solver_->x, 2) << ", " <<
          "f(x) = " << gsl_vector_get(solver_->f, 0) << " " <<
                       gsl_vector_get(solver_->f, 1) << " " <<
                       gsl_vector_get(solver_->f, 2) << std::endl;
}


}  // namespace Smash