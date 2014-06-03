/*
 *
 *    Copyright (c) 2013-2014
 *      SMASH Team
 *
 *    GNU General Public License (GPLv3 or later)
 *
 */
#include "include/resonances.h"

#include <gsl/gsl_integration.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_sf_coupling.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

#include "include/constants.h"
#include "include/decaymodes.h"
#include "include/distributions.h"
#include "include/fourvector.h"
#include "include/macros.h"
#include "include/outputroutines.h"
#include "include/particles.h"
#include "include/particledata.h"
#include "include/particletype.h"
#include "include/processbranch.h"
#include "include/random.h"

namespace Smash {

/* Calculate isospin Clebsch-Gordan coefficient
 * (-1)^(j1 - j2 + m3) * sqrt(2 * j3 + 1) * [Wigner 3J symbol]
 * Note that the calculation assumes that isospin values
 * have been multiplied by two
 */
double clebsch_gordan_coefficient(const int isospin_a,
  const int isospin_b, const int isospin_resonance,
  const int isospin_z_a, const int isospin_z_b,
  const int isospin_z_resonance) {
  double wigner_3j =  gsl_sf_coupling_3j(isospin_a,
     isospin_b, isospin_resonance,
     isospin_z_a, isospin_z_b, -isospin_z_resonance);
  double clebsch_gordan_isospin = 0.0;
  if (std::abs(wigner_3j) > really_small)
    clebsch_gordan_isospin = pow(-1, isospin_a / 2.0
    - isospin_b / 2.0 + isospin_z_resonance / 2.0)
    * sqrt(isospin_resonance + 1) * wigner_3j;

  printd("CG: %g I1: %i I2: %i IR: %i iz1: %i iz2: %i izR: %i \n",
       clebsch_gordan_isospin, isospin_a, isospin_b,
       isospin_resonance, isospin_z_a, isospin_z_b,
       isospin_z_resonance);

  return clebsch_gordan_isospin;
}

/**
 * Function for 1-dimensional GSL integration.
 *
 * \param[in] integrand_function Function of 1 variable to be integrated over.
 * \param[in] parameters Container for possible parameters
 * needed by the integrand.
 * \param[in] lower_limit Lower limit of the integral.
 * \param[in] upper_limit Upper limit of the integral.
 * \param[out] integral_value Result of integration.
 * \param[out] integral_error Uncertainty of the result.
 */
static void quadrature_1d(double (*integrand_function)(double, void *),
                          std::vector<double> *parameters, double lower_limit,
                          double upper_limit, double *integral_value,
                          double *integral_error);

/* calculate_minimum_mass
 * - calculate the minimum rest energy the resonance must have
 * to be able to decay through any of its decay channels
 * NB: This function assumes stable decay products!
 */
float calculate_minimum_mass(const Particles &particles, PdgCode pdgcode) {
  /* If the particle happens to be stable, just return the mass */
  if (particles.particle_type(pdgcode).width() < 0.0) {
    return particles.particle_type(pdgcode).mass();
  }
  /* Otherwise, let's find the highest mass value needed in any decay mode */
  float minimum_mass = 0.0;
  const std::vector<ProcessBranch> decaymodes =
      particles.decay_modes(pdgcode).decay_mode_list();
  for (std::vector<ProcessBranch>::const_iterator mode = decaymodes.begin();
       mode != decaymodes.end(); ++mode) {
    size_t decay_particles = mode->pdg_list().size();
    float total_mass = 0.0;
    for (size_t i = 0; i < decay_particles; i++) {
      /* Stable decay products assumed; for resonances the mass can be lower! */
      total_mass += particles.particle_type(mode->pdg_list().at(i)).mass();
    }
    if (total_mass > minimum_mass)
      minimum_mass = total_mass;
  }
  return minimum_mass;
}

/* resonance_cross_section - energy-dependent cross section
 * for producing a resonance
 */
std::vector<ProcessBranch> resonance_cross_section(
    const ParticleData &particle1, const ParticleData &particle2,
    const ParticleType &type_particle1, const ParticleType &type_particle2,
    const Particles &particles) {
  std::vector<ProcessBranch> resonance_process_list;

  /* Isospin symmetry factor, by default 1 */
  int symmetryfactor = 1;
  // the isospin symmetry factor is 2 if both particles are in the same
  // isospin multiplet:
  if (type_particle1.pdgcode().iso_multiplet()
   == type_particle2.pdgcode().iso_multiplet()) {
    symmetryfactor = 2;
  }

  /* Mandelstam s = (p_a + p_b)^2 = square of CMS energy */
  const double mandelstam_s =
       (particle1.momentum() + particle2.momentum()).Dot(
         particle1.momentum() + particle2.momentum());

  /* CM momentum */
  const double cm_momentum_squared
    = (particle1.momentum().Dot(particle2.momentum())
       * particle1.momentum().Dot(particle2.momentum())
       - type_particle1.mass() * type_particle1.mass()
       * type_particle2.mass() * type_particle2.mass()) / mandelstam_s;

  /* Find all the possible resonances */
  for (const ParticleType &type_resonance : particles.types()) {
    /* Not a resonance, go to next type of particle */
    if (type_resonance.width() < 0.0) {
      continue;
    }

    /* Same resonance as in the beginning, ignore */
    if ((type_particle1.width() > 0.0
         && type_resonance.pdgcode() == type_particle1.pdgcode())
        || (type_particle2.width() > 0.0
            && type_resonance.pdgcode() == type_particle2.pdgcode())) {
      continue;
    }

    /* No decay channels found, ignore */
    if (particles.decay_modes(type_resonance.pdgcode()).is_empty()) {
      continue;
    }

    float resonance_xsection
      = symmetryfactor * two_to_one_formation(particles, type_particle1,
        type_particle2, type_resonance, mandelstam_s, cm_momentum_squared);

    /* If cross section is non-negligible, add resonance to the list */
    if (resonance_xsection > really_small) {
      resonance_process_list.push_back(ProcessBranch(type_resonance.pdgcode(),
                                                     resonance_xsection,1));

      printd("Found resonance %s (%s) with mass %f and width %f.\n",
             type_resonance.pdgcode().string().c_str(),
             type_resonance.name().c_str(),
             type_resonance.mass(), type_resonance.width());
      printd("2->1 with original particles: %s %s Charges: %i %i \n",
             type_particle1.name().c_str(), type_particle2.name().c_str(),
             type_particle1.charge(), type_particle2.charge());
    }
    /* Same procedure for possible 2->2 resonance formation processes */
    /* XXX: For now, we allow this only for baryon-baryon interactions */
    if (type_particle1.spin() % 2 != 0 && type_particle2.spin() % 2 != 0) {
      size_t two_to_two_processes
         = two_to_two_formation(particles, type_particle1,
           type_particle2, type_resonance, mandelstam_s, cm_momentum_squared,
           &resonance_process_list);
      if (two_to_two_processes > 0) {
        printd("Found %zu 2->2 processes for resonance %s (%s).\n",
               two_to_two_processes,
               type_resonance.pdgcode().string().c_str(),
               type_resonance.name().c_str());
        printd("2->2 with original particles: %s %s Charges: %i %i \n",
               type_particle1.name().c_str(), type_particle2.name().c_str(),
               type_particle1.charge(), type_particle2.charge());
      }
    }
  }
  return resonance_process_list;
}

/* two_to_one_formation -- only the resonance in the final state */
double two_to_one_formation(const Particles &particles,
                            const ParticleType &type_particle1,
                            const ParticleType &type_particle2,
                            const ParticleType &type_resonance,
                            double mandelstam_s, double cm_momentum_squared) {
  /* Check for charge conservation */
  if (type_resonance.charge() != type_particle1.charge()
                                 + type_particle2.charge())
    return 0.0;

  /* Check for baryon number conservation */
  if (type_particle1.spin() % 2 != 0 || type_particle2.spin() % 2 != 0) {
    /* Step 1: We must have fermion */
    if (type_resonance.spin() % 2 == 0) {
      return 0.0;
    }
    /* Step 2: We must have antiparticle for antibaryon
     * (and non-antiparticle for baryon)
     */
    if (type_particle1.pdgcode().baryon_number() != 0
        && (type_particle1.pdgcode().baryon_number()
            != type_resonance.pdgcode().baryon_number())) {
      return 0.0;
    } else if (type_particle2.pdgcode().baryon_number() != 0
        && (type_particle2.pdgcode().baryon_number()
        != type_resonance.pdgcode().baryon_number())) {
      return 0.0;
    }
  }

  double clebsch_gordan_isospin
    = clebsch_gordan_coefficient(type_particle1.isospin(),
	 type_particle2.isospin(), type_resonance.isospin(),
	 type_particle1.pdgcode().isospin3(),
         type_particle2.pdgcode().isospin3(),
         type_resonance.pdgcode().isospin3());

  /* If Clebsch-Gordan coefficient is zero, don't bother with the rest */
  if (std::abs(clebsch_gordan_isospin) < really_small)
    return 0.0;

  /* Check the decay modes of this resonance */
  const std::vector<ProcessBranch> decaymodes
    = particles.decay_modes(type_resonance.pdgcode()).decay_mode_list();
  bool not_enough_energy = false;
  /* Detailed balance required: Formation only possible if
   * the resonance can decay back to these particles
   */
  bool not_balanced = true;
  for (std::vector<ProcessBranch>::const_iterator mode
       = decaymodes.begin(); mode != decaymodes.end(); ++mode) {
    size_t decay_particles = mode->pdg_list().size();
    if ( decay_particles > 3 ) {
      printf("Warning: Not a 1->2 or 1->3 process!\n");
      printf("Number of decay particles: %zu \n", decay_particles);
    } else {
      /* There must be enough energy to produce all decay products */
      float mass_a, mass_b, mass_c = 0.0;
      mass_a = calculate_minimum_mass(particles, mode->pdg_list().at(0));
      mass_b = calculate_minimum_mass(particles, mode->pdg_list().at(1));
      if (decay_particles == 3) {
        mass_c = calculate_minimum_mass(particles, mode->pdg_list().at(2));
      }
      if (sqrt(mandelstam_s) < mass_a + mass_b + mass_c)
        not_enough_energy = true;
      /* Initial state is also a possible final state;
       * weigh the cross section with the ratio of this branch
       * XXX: For now, assuming only 2-particle initial states
       */
      if (decay_particles == 2
          && ((mode->pdg_list().at(0) == type_particle1.pdgcode()
               && mode->pdg_list().at(1) == type_particle2.pdgcode())
              || (mode->pdg_list().at(0) == type_particle2.pdgcode()
                  && mode->pdg_list().at(1) == type_particle1.pdgcode()))
          && (mode->weight() > 0.0))
        not_balanced = false;
    }
  }
  if (not_enough_energy)
    return 0.0;

  if (not_balanced)
    return 0.0;

  /* Calculate spin factor */
  const double spinfactor = (type_resonance.spin() + 1)
    / ((type_particle1.spin() + 1) * (type_particle2.spin() + 1));
  float resonance_width = type_resonance.width();
  float resonance_mass = type_resonance.mass();
  /* Calculate resonance production cross section
   * using the Breit-Wigner distribution as probability amplitude
   * See Eq. (176) in Buss et al., Physics Reports 512, 1 (2012)
   */
  return clebsch_gordan_isospin * clebsch_gordan_isospin * spinfactor
         * 4.0 * M_PI / cm_momentum_squared
         * breit_wigner(mandelstam_s, resonance_mass, resonance_width)
         * hbarc * hbarc / fm2_mb;
}

/* two_to_two_formation -- resonance and another particle in final state */
size_t two_to_two_formation(const Particles &particles,
  const ParticleType &type_particle1,
  const ParticleType &type_particle2, const ParticleType &type_resonance,
  double mandelstam_s, double cm_momentum_squared,
  std::vector<ProcessBranch> *process_list) {
  size_t number_of_processes = 0;
  /* If we have two baryons in the beginning, we must have fermion resonance */
  if (type_particle1.pdgcode().baryon_number() != 0
   && type_particle2.pdgcode().baryon_number() != 0
   && ! type_particle1.pdgcode().is_antiparticle_of(type_particle2.pdgcode())
   && type_resonance.pdgcode().baryon_number() == 0)
    return 0.0;

  /* Isospin z-component based on Gell-Mann–Nishijima formula
   * 2 * Iz = 2 * charge - (baryon number + strangeness + charm)
   * XXX: Strangeness and charm ignored for now!
   */
  const int isospin_z_resonance = type_resonance.pdgcode().isospin3();

  /* Compute initial total combined isospin range */
  int initial_total_maximum
    = type_particle1.isospin() + type_particle2.isospin();
  int initial_total_minimum
    = abs(type_particle1.isospin() - type_particle2.isospin());
  /* Loop over particle types to find allowed combinations */
  for (const ParticleType &second_type : particles.types()) {
    /* We are interested only stable particles here */
    if (second_type.width() > 0.0) {
      continue;
    }

    /* Check for charge conservation */
    if (type_resonance.charge() + second_type.charge()
        != type_particle1.charge() + type_particle2.charge()) {
      continue;
    }

    /* Check for baryon number conservation */
    int initial_baryon_number = type_particle1.pdgcode().baryon_number()
                              + type_particle2.pdgcode().baryon_number();
    int final_baryon_number = type_resonance.pdgcode().baryon_number()
                            + second_type.pdgcode().baryon_number();
    if (final_baryon_number != initial_baryon_number) {
      continue;
    }

    /* Compute total isospin range with given initial and final particles */
    int isospin_maximum = std::min(type_resonance.isospin()
      + second_type.isospin(), initial_total_maximum);
    int isospin_minimum = std::max(abs(type_resonance.isospin()
      - second_type.isospin()), initial_total_minimum);

    int isospin_z_i = second_type.pdgcode().isospin3();
    int isospin_z_final = isospin_z_resonance + isospin_z_i;

    int isospin_final = isospin_maximum;
    double clebsch_gordan_isospin = 0.0;
    while (isospin_final >= isospin_minimum) {
      if (abs(isospin_z_final) > isospin_final) {
        break;
      }
      clebsch_gordan_isospin
        = clebsch_gordan_coefficient(type_resonance.isospin(),
            second_type.isospin(), isospin_final,
            isospin_z_resonance, isospin_z_i, isospin_z_final);

      /* isospin is multiplied by 2,
       *  so we must also decrease it by increments of 2
       */
      isospin_final = isospin_final - 2;
    }
    /* If Clebsch-Gordan coefficient is zero, don't bother with the rest */
    if (fabs(clebsch_gordan_isospin) < really_small) {
      continue;
    }

    /* Check the decay modes of this resonance */
    const std::vector<ProcessBranch> decaymodes
      = particles.decay_modes(type_resonance.pdgcode()).decay_mode_list();
    bool not_enough_energy = false;
    double minimum_mass = 0.0;
    for (std::vector<ProcessBranch >::const_iterator mode
         = decaymodes.begin(); mode != decaymodes.end(); ++mode) {
      size_t decay_particles = mode->pdg_list().size();
      if ( decay_particles > 3 ) {
        printf("Warning: Not a 1->2 or 1->3 process!\n");
        printf("Number of decay particles: %zu \n", decay_particles);
      } else {
        /* There must be enough energy to produce all decay products */
        float mass_a, mass_b, mass_c = 0.0;
        mass_a = calculate_minimum_mass(particles, mode->pdg_list().at(0));
        mass_b = calculate_minimum_mass(particles, mode->pdg_list().at(1));
        if (decay_particles == 3) {
          mass_c = calculate_minimum_mass(particles, mode->pdg_list().at(2));
        }
        if (sqrt(mandelstam_s) < mass_a + mass_b + mass_c
                                 + second_type.mass()) {
          not_enough_energy = true;
        } else if (minimum_mass < mass_a + mass_b + mass_c) {
          minimum_mass = mass_a + mass_b + mass_c;
        }
      }
    }
    if (not_enough_energy) {
      continue;
    }

    /* Calculate resonance production cross section
     * using the Breit-Wigner distribution as probability amplitude
     * Integrate over the allowed resonance mass range
     */
    std::vector<double> integrand_parameters;
    integrand_parameters.push_back(type_resonance.width());
    integrand_parameters.push_back(type_resonance.mass());
    integrand_parameters.push_back(second_type.mass());
    integrand_parameters.push_back(mandelstam_s);
    double lower_limit = minimum_mass;
    double upper_limit = (sqrt(mandelstam_s) - second_type.mass());
    printd("Process: %s %s -> %s %s\n", type_particle1.name().c_str(),
     type_particle2.name().c_str(), second_type.name().c_str(),
     type_resonance.name().c_str());
    printd("Limits: %g %g \n", lower_limit, upper_limit);
    double resonance_integral, integral_error;
    quadrature_1d(&spectral_function_integrand, &integrand_parameters,
                  lower_limit, upper_limit,
                  &resonance_integral, &integral_error);
    printd("Integral value: %g Error: %g \n", resonance_integral,
      integral_error);

    /* matrix element squared over 16pi (in mb GeV^2)
     * (uniform angular distribution assumed)
     */
    double matrix_element = 180;

    /* Cross section for 2->2 process with resonance in final state.
     * Based on the general differential form in
     * Buss et al., Physics Reports 512, 1 (2012), Eq. (D.28)
     */
    float xsection = clebsch_gordan_isospin * clebsch_gordan_isospin
                      * matrix_element
                      / mandelstam_s
                      / sqrt(cm_momentum_squared)
                      * resonance_integral;

    if (xsection > really_small) {
      process_list->push_back(ProcessBranch(type_resonance.pdgcode(),
                                            second_type.pdgcode(),xsection,1));
      number_of_processes++;
    }
  }
  return number_of_processes;
}

/* Function for 1-dimensional GSL integration  */
void quadrature_1d(double (*integrand_function)(double, void*),
                     std::vector<double> *parameters,
                     double lower_limit, double upper_limit,
                     double *integral_value, double *integral_error) {
  gsl_integration_workspace *workspace
    = gsl_integration_workspace_alloc(1000);
  gsl_function integrand;
  integrand.function = integrand_function;
  integrand.params = parameters;
  size_t subintervals_max = 100;
  int gauss_points = 2;
  double accuracy_absolute = 1.0e-6;
  double accuracy_relative = 1.0e-4;

  gsl_integration_qag(&integrand, lower_limit, upper_limit,
                      accuracy_absolute, accuracy_relative,
                      subintervals_max, gauss_points, workspace,
                      integral_value, integral_error);

  gsl_integration_workspace_free(workspace);
}

/* Spectral function of the resonance */
double spectral_function(double resonance_mass, double resonance_pole,
                         double resonance_width) {
  /* breit_wigner is essentially pi * mass * width * spectral function
   * (mass^2 is called mandelstam_s in breit_wigner)
   */
  return breit_wigner(resonance_mass * resonance_mass,
                      resonance_pole, resonance_width)
         / M_PI / resonance_mass / resonance_width;
}

/* Spectral function integrand for GSL integration */
double spectral_function_integrand(double resonance_mass,
                                   void *parameters) {
  std::vector<double> *integrand_parameters
    = reinterpret_cast<std::vector<double> *>(parameters);
  double resonance_width = integrand_parameters->at(0);
  double resonance_pole_mass = integrand_parameters->at(1);
  double stable_mass = integrand_parameters->at(2);
  double mandelstam_s = integrand_parameters->at(3);

  /* center-of-mass momentum of final state particles */
  if (mandelstam_s - (stable_mass + resonance_mass)
      * (stable_mass + resonance_mass) > 0.0) {
    double cm_momentum_final
      = sqrt((mandelstam_s - (stable_mass + resonance_mass)
              * (stable_mass + resonance_mass))
             * (mandelstam_s - (stable_mass - resonance_mass)
                * (stable_mass - resonance_mass))
             / (4 * mandelstam_s));

    /* Integrand is the spectral function weighted by the
     * CM momentum of final state
     * In addition, dm^2 = 2*m*dm
     */
    return spectral_function(resonance_mass, resonance_pole_mass,
                             resonance_width)
           * cm_momentum_final
           * 2 * resonance_mass;
  } else {
    return 0.0;
  }
}

/* Resonance mass sampling for 2-particle final state */
double sample_resonance_mass(const Particles &particles, PdgCode pdg_resonance,
                             PdgCode pdg_stable, double cms_energy) {
  /* First, find the minimum mass of this resonance */
  double minimum_mass = calculate_minimum_mass(particles, pdg_resonance);
  /* Define distribution parameters */
  float mass_stable = particles.particle_type(pdg_stable).mass();
  std::vector<double> parameters;
  parameters.push_back(particles.particle_type(pdg_resonance).width());
  parameters.push_back(particles.particle_type(pdg_resonance).mass());
  parameters.push_back(mass_stable);
  parameters.push_back(cms_energy * cms_energy);

  /* sample resonance mass from the distribution
   * used for calculating the cross section
   */
  double mass_resonance = 0.0, random_number = 1.0;
  double distribution_max
    = spectral_function_integrand(parameters.at(0), &parameters);
  double distribution_value = 0.0;
  while (random_number > distribution_value) {
    random_number = Random::uniform(0.0, distribution_max);
    mass_resonance = Random::uniform(minimum_mass, cms_energy - mass_stable);
    distribution_value
      = spectral_function_integrand(mass_resonance, &parameters);
  }
  return mass_resonance;
}

}  // namespace Smash
