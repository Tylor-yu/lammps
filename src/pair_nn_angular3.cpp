/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author:  Yongnan Xiong (HNU), xyn@hnu.edu.cn
                         Aidan Thompson (SNL)
------------------------------------------------------------------------- */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pair_nn_angular3.h"
#include "atom.h"
#include "neighbor.h"
#include "neigh_request.h"
#include "force.h"
#include "comm.h"
#include "memory.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "memory.h"
#include "error.h"
#include <fenv.h>

#include <fstream>
#include <iomanip>

using namespace LAMMPS_NS;
using std::cout;
using std::endl;

//#define MAXLINE 1024
//#define DELTA 4

/* ---------------------------------------------------------------------- */

PairNNAngular3::PairNNAngular3(LAMMPS *lmp) : Pair(lmp)
{
  single_enable = 0; // We don't provide the force between two atoms only since it is Angular3
  restartinfo = 0;   // We don't write anything to restart file
  one_coeff = 1;     // only one coeff * * call
  manybody_flag = 1; // Not only a pair style since energies are computed from more than one neighbor
  cutoff = 10.0;      // Will be read from command line
}

/* ----------------------------------------------------------------------
   check if allocated, since class can be destructed when incomplete
------------------------------------------------------------------------- */

PairNNAngular3::~PairNNAngular3()
{
  if (copymode) return;
  // If you allocate stuff you should delete and deallocate here. 
  // Allocation of normal vectors/matrices (not armadillo), should be created with
  // memory->create(...)

  if (allocated) {
    memory->destroy(setflag);
    memory->destroy(cutsq);
  }
}

/* ---------------------------------------------------------------------- */

double PairNNAngular3::network(arma::mat inputVector) {
    // inputGraph vector is a 1xinputGraphs vector

    // linear activation for inputGraph layer
    m_preActivations[0] = inputVector;
    m_activations[0] = m_preActivations[0];

    // hidden layers
    for (int i=0; i < m_nLayers; i++) {
        // weights and biases starts at first hidden layer:
        // weights[0] are the weights connecting inputGraph layer to first hidden layer
        m_preActivations[i+1] = m_activations[i]*m_weights[i] + m_biases[i];
        m_activations[i+1] = sigmoid(m_preActivations[i+1]);
    }

    // linear activation for output layer
    m_preActivations[m_nLayers+1] = m_activations[m_nLayers]*m_weights[m_nLayers] + m_biases[m_nLayers];
    m_activations[m_nLayers+1] = m_preActivations[m_nLayers+1];

    // return activation of output neuron
    return m_activations[m_nLayers+1](0,0);
}

arma::mat PairNNAngular3::backPropagation() {
  // find derivate of output w.r.t. intput, i.e. dE/dr_ij
  // need to find the "error" terms for all the nodes in all the layers

  // the derivative of the output neuron's activation function w.r.t.
  // its inputGraph is propagated backwards.
  // the output activation function is f(x) = x, so this is 1
  arma::mat output(1,1); output.fill(1);
  m_derivatives[m_nLayers+1] = output;

  // we can thus compute the error vectors for the other layers
  for (int i=m_nLayers; i > 0; i--) {
      m_derivatives[i] = ( m_derivatives[i+1]*m_weightsTransposed[i] ) %
                         sigmoidDerivative(m_preActivations[i]);
  }

  // linear activation function for inputGraph neurons
  m_derivatives[0] = m_derivatives[1]*m_weightsTransposed[0];

  return m_derivatives[0];
}

arma::mat PairNNAngular3::sigmoid(arma::mat matrix) {

  return 1.0/(1 + arma::exp(-matrix));
}

arma::mat PairNNAngular3::sigmoidDerivative(arma::mat matrix) {

  arma::mat sigmoidMatrix = sigmoid(matrix);
  return sigmoidMatrix % (1 - sigmoidMatrix);
}

arma::mat PairNNAngular3::Fc(arma::mat R, double Rc, bool cut) {

  arma::mat value = 0.5*(arma::cos(m_pi*R/Rc) + 1);

  if (cut)
    for (int i=0; i < arma::size(R)(1); i++)
      if (R(0,i) > Rc) 
        value(0,i) = 0;
  
  return value;
}

double PairNNAngular3::Fc(double R, double Rc, bool cut) {

  if (cut)
    if (R < Rc)
      return 0.5*(cos(m_pi*R/Rc) + 1);
    else
      return 0;
  else
    return 0.5*(cos(m_pi*R/Rc) + 1);
}

arma::mat PairNNAngular3::dFcdR(arma::mat R, double Rc, bool cut) {

  double Rcinv = 1.0/Rc;
  arma::mat value = -(0.5*m_pi*Rcinv) * arma::sin(m_pi*R*Rcinv);

  if (cut)
    for (int i=0; i < arma::size(R)(1); i++)
      if (R(0,i) > Rc) 
        value(0,i) = 0;
  
  return value; 
}

double PairNNAngular3::dFcdR(double R, double Rc, bool cut) {

  double Rcinv = 1.0/Rc;

  if (cut)
    if (R < Rc)
      return -(0.5*m_pi*Rcinv) * sin(m_pi*R*Rcinv);
    else
      return 0;
  else
    return -(0.5*m_pi*Rcinv) * sin(m_pi*R*Rcinv);
}

double PairNNAngular3::G1(arma::mat Rij, double Rc) {

  return arma::accu( Fc(Rij, Rc, false) );
}

arma::mat PairNNAngular3::dG1dR(arma::mat Rij, double Rc) {

  return dFcdR(Rij, Rc, false);
}

double PairNNAngular3::G2(double rij, double eta, double Rc, double Rs) {

  return exp(-eta*(rij - Rs)*(rij - Rs)) * Fc(rij, Rc, false);
}

double PairNNAngular3::dG2dR(double Rij, double eta, double Rc, double Rs) {

  return -exp(-eta*(Rij - Rs)*(Rij - Rs)) *
          ( 2*eta*(Rs - Rij) * Fc(Rij, Rc, false) + dFcdR(Rij, Rc, false) ) / Rij;
}

double PairNNAngular3::G4(double rij, double rik, double rjk, double cosTheta, 
                          double eta, double Rc, double zeta, double lambda) {

  return pow(2, 1-zeta) * 
         pow(1 + lambda*cosTheta, zeta) *  
         exp( -eta*(rij*rij + rik*rik + rjk*rjk) ) * 
         Fc(rij, Rc, false) * Fc(rik, Rc, false) * Fc(rjk, Rc, true);
}


void PairNNAngular3::dG4dR(double Rij, double Rik, double Rjk, 
                           double cosTheta, double eta, double Rc, 
                           double zeta, double lambda,
                           arma::mat &dEdRj3, arma::mat &dEdRk3,
                           double xi, double yi, double zi) {

  double powCosThetaM1 = pow(2, 1-zeta)*pow(1 + lambda*cosTheta, zeta-1);
  double F1 = powCosThetaM1 * (1 + lambda*cosTheta);

  double F2 = exp(-eta*(Rij*Rij + Rik*Rik + Rjk*Rjk));

  double FcRij = Fc(Rij, Rc, false);
  double FcRik = Fc(Rik, Rc, false);
  double FcRjk = Fc(Rjk, Rc, true);
  double F3 = FcRij * FcRik * FcRjk;

  double K = lambda*zeta*powCosThetaM1;
  double L = -2*eta*F2;
  double Mij = dFcdR(Rij, Rc, false);
  double Mik = dFcdR(Rik, Rc, false);
  double Mjk = dFcdR(Rjk, Rc, true);

  double term1 = K * F2 * F3;
  double term2 = F1 * L * F3;

  double F1F2 = F1 * F2;
  double term3ij = F1F2 * FcRik * FcRjk * Mij;
  double term3ik = F1F2 * FcRjk * Mik * FcRij;

  double termjk = F1F2 * Mjk * FcRik * FcRij;

  double RijInv = 1.0 / Rij;
  double cosRijInv2 = cosTheta * RijInv*RijInv;

  double RikInv = 1.0 / Rik;
  double cosRikInv2 = cosTheta * RikInv*RikInv;

  double RijRikInv = RijInv * RikInv;

  double termij = (cosRijInv2 * term1) - term2 - RijInv*term3ij;
  double termik = (cosRikInv2 * term1) - term2 - RikInv*term3ik;
  double crossTerm = -term1 * RijRikInv; 
  double crossTermjk = termjk / Rjk;


  // all k's give a triplet energy contributon to atom j
  /*dEdRj3.row(0) =  (drij(0,0) * termij) + (drik.row(0) * crossTerm) -  
                   (drjk.row(0) * crossTermjk);
  dEdRj3.row(1) =  (drij(1,0) * termij) + (drik.row(1) * crossTerm) -  
                   (drjk.row(1) * crossTermjk);
  dEdRj3.row(2) =  (drij(2,0) * termij) + (drik.row(2) * crossTerm) -  
                   (drjk.row(2) * crossTermjk);

  // need all the different components of k's forces to do sum k > j
  dEdRk3.row(0) =  (drik.row(0) * termik) + (drij(0,0) * crossTerm) +  
                   (drjk.row(0) * crossTermjk);
  dEdRk3.row(1) =  (drik.row(1) * termik) + (drij(1,0) * crossTerm) +  
                   (drjk.row(1) * crossTermjk);
  dEdRk3.row(2) =  (drik.row(2) * termik) + (drij(2,0) * crossTerm) +  
                   (drjk.row(2) * crossTermjk);*/
}

void PairNNAngular3::compute(int eflag, int vflag)
{
  feenableexcept(FE_INVALID | FE_OVERFLOW);

  double evdwl = 0.0;
  eng_vdwl = 0.0;
  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = vflag_fdotr = 0;

  double **x = atom->x;
  double **f = atom->f;
  tagint *tag = atom->tag;
  int nlocal = atom->nlocal;    // atoms belonging to current processor
  int newton_pair = force->newton_pair; // decides how energy and virial are tallied

  int inum = list->inum;
  int *ilist = list->ilist;
  int *numneigh = list->numneigh;
  int **firstneigh = list->firstneigh;

  // store all NN derivatives for force calculations later
  std::vector<arma::mat> NNderivatives(inum);

  // store all atoms below cutoff for each i
  std::vector<std::vector<int>> neighShort(inum);

  // store all Rij
  std::vector<std::vector<double>> Rij(inum);

  // store Rik, Rjk, cosTheta as arma matrices
  std::vector<std::vector<arma::mat>> Riks(inum);
  std::vector<std::vector<arma::mat>> cosThetas(inum);
  std::vector<std::vector<arma::mat>> Rjks(inum);

  // store k tags
  std::vector<std::vector<std::vector<int>>> tagsk(inum);

  // loop over full neighbor list of my atoms
  for (int ii = 0; ii < inum; ii++) {
    
    int i = ilist[ii];
    tagint itag = tag[i];

    double xi = x[i][0];
    double yi = x[i][1];
    double zi = x[i][2];

    // two-body interactions, skip half of them
    int *jlist = firstneigh[i];
    int jnum = numneigh[i];

    tagsk[ii].resize(jnum);

    // input vector to NN
    arma::mat inputVector(1, m_numberOfSymmFunc, arma::fill::zeros);

    // keep track of neighbours
    int neighbours = 0;

    // collect all pairs
    for (int jj = 0; jj < jnum; jj++) {

      int j = jlist[jj];
      j &= NEIGHMASK;
      tagint jtag = tag[j];

      double xij = xi - x[j][0];
      double yij = yi - x[j][1];
      double zij = zi - x[j][2];

      double rsq1 = xij*xij + yij*yij + zij*zij;

      if (rsq1 >= cutoff*cutoff) continue;

      // store neighbour
      neighShort[neighbours].push_back(j);

      // store pair coordinates
      double rij = sqrt(rsq1);
      Rij[ii].push_back(rij);

      // apply 2-body symmetry
      int s = 0;
      for (auto param : m_parameters) {
        if ( param.size() == 3 ) 
          inputVector(0,s) += G2(rij, param[0], param[1], param[2]);
        s++;
      }

      // collect triplets for this (i,j)
      arma::mat Rik(1, jnum-1);
      arma::mat CosTheta(1, jnum-1);
      arma::mat Rjk(1, jnum-1); 

      // three-body
      int neighk = 0;
      for (int kk = jj+1; kk < jnum; kk++) {

        int k = jlist[kk];
        k &= NEIGHMASK;
        tagint ktag = tag[k];

        //if (j == k) continue;

        double xik = xi - x[k][0];
        double yik = yi - x[k][1];
        double zik = zi - x[k][2];

        double rsq2 = xik*xik + yik*yik + zik*zik;  

        if (rsq2 >= cutoff*cutoff) continue;
        
        // calculate quantites needed in G4
        double rik = sqrt(rsq2);
        double cosTheta = ( xij*xik + yij*yik + zij*zik ) / (rij*rik);

        double xjk = x[j][0] - x[k][0];
        double yjk = x[j][1] - x[k][1];
        double zjk = x[j][2] - x[k][2];

        double rjk = sqrt(xjk*xjk + yjk*yjk + zjk*zjk);

        // collect triplets
        Rik(0,neighk) = rik;
        CosTheta(0, neighk) = cosTheta;
        Rjk(0, neighk) = rjk;
        tagsk[neighbours][neighk].push_back(k);
        neighk++;

        // apply 3-body symmetry
        s = 0;
        for (auto param : m_parameters) {
          if ( param.size() == 4 ) 
            inputVector(0,s) += G4(rij, rik, rjk, cosTheta,
                                   param[0], param[1], param[2], param[3]);
          s++;
        }
      }

      // get rid of empty elements
      Rik = Rik.head_cols(neighk);
      CosTheta = CosTheta.head_cols(neighk);
      Rjk = Rjk.head_cols(neighk);

      // store all k's for curren (i,j) to compute forces later
      Riks[neighbours].push_back(Rik);
      cosThetas[neighbours].push_back(CosTheta);
      Rjks[neighbours].push_back(Rjk);
      neighbours++;
    }

    // apply NN to get energy
    evdwl = network(inputVector);

    // store energy of ato i
    eatom[i] += evdwl;

    // set energy manually (not use ev_tally for energy)
    eng_vdwl += evdwl;

    // backpropagate to obtain gradient of NN and store
    arma::mat dEdG = backPropagation();
    NNderivatives[ii] = dEdG;
  }

  // force calculations
  // for each i: loop through neighShort list and calculate the derivatives
  // of all the neighbour energies w.r.t. i's coordinates

  for (int ii = 0; ii < inum; ii++) {

    int i = ilist[ii];
    tagint itag = tag[i];

    double xi = x[i][0];
    double yi = x[i][1];
    double zi = x[i][2];

    double fxtmp = 0;
    double fytmp = 0;
    double fztmp = 0;

    // two-body interactions, skip half of them
    int jnum = neighShort[i].size();

    // loop through neighbours
    for (int jj = 0; jj < jnum; jj++) {

      int j = neighShort[ii][jj];
      j &= NEIGHMASK;
      tagint jtag = tag[j];

      // loop through all symmetry functions of atom j
      // (all symmetry vectors are equal for the time being)
      for (auto param : m_parameters) {

        // this should 
        double rij = Rij[ii][jj];

        // G2
        if ( param.size() == 3 ) {

          // find derivative of this G2 w.r.t. coordinates of atom i
          double symDiff = dG2dR(rij, param[0], param[1], param[2]);

          /*double fpair = -NNderivatives[j] * symDiff;

          // is this the force on atom j or i???
          f[j][0] += fpair*xi;
          f[j][1] += fpair*yi;
          f[j][2] += fpair*zi;*/
        }

        // G4
        if ( param.size() == 4 ) {

          int numberOfTriplets = arma::size(Riks[ii][jj])(1);

          arma::mat dEdRj3(3, numberOfTriplets);
          arma::mat dEdRk3(3, numberOfTriplets); // triplet force for all atoms k

          double fj3[3];
          double fk3[3];
          for (int kk=0; kk < numberOfTriplets; kk++) {

            // find derivative of this G4 w.r.t. coordinates of atom i
            /*dG4dR(rij, Riks[ii][jj](0,kk), Rjks[ii][jj](0,kk), 
                  cosThetas[ii][jj](0,kk),
                  param[0], param[1], param[2], param[3], 
                  dEdRj3, dEdRk3, xi, yi, zi);

            // triplet force j
            fj3[0] = NNderivatives[j] * dEdRj3(0,kk);
            fj3[1] = NNderivatives[j] * dEdRj3(1,kk);
            fj3[2] = NNderivatives[j] * dEdRj3(2,kk);

            // triplet force k
            fk3[0] = NNderivatives[tagsk[ii][jj][kk]] * dEdRk3(0,kk);
            fk3[1] = NNderivatives[tagsk[ii][jj][kk]] * dEdRk3(1,kk);
            fk3[2] = NNderivatives[tagsk[ii][jj][kk]] * dEdRk3(2,kk);

            fxtmp += fpair*xi;
            fytmp += fpair*yi;
            fztmp += fpair*zi;*/
          }
        }
      }
    }
  }

    /*double fx2 = 0;
    double fy2 = 0;
    double fz2 = 0;

    double fx3j = 0;
    double fy3j = 0;
    double fz3j = 0;
    double fx3k = 0;
    double fy3k = 0;
    double fz3k = 0;
    
    // calculate forces by differentiating the symmetry functions
    // UNTRUE(?): dEdR(j) will be the force contribution from atom j on atom i
    for (int s=0; s < m_numberOfSymmFunc; s++) {
      
      // G2: one atomic pair environment per symmetry function
      if ( m_parameters[s].size() == 3 ) {

        arma::mat dG2(1,neighbours); // derivative of G2

        // calculate cerivative of G2 for all pairs
        // pass dG2 by reference instead of coyping matrices
        // and returning from function --> speed-up
        dG2dR(Rij, m_parameters[s][0],
              m_parameters[s][1], m_parameters[s][2], dG2);

        // chain rule. all pair foces
        arma::mat fpairs = -dEdG(0,s) * dG2 / Rij;

        // loop through all pairs for N3L
        for (int l=0; l < neighbours; l++) {
          double fpair = fpairs(0,l);
          fx2 += fpair*drij(0,l);
          fy2 += fpair*drij(1,l);
          fz2 += fpair*drij(2,l);

          // NOT N3L NOW
          //f[tagsj[l]][0] -= fpair*drij(0,l);
          //f[tagsj[l]][1] -= fpair*drij(1,l);
          //f[tagsj[l]][2] -= fpair*drij(2,l);

          if (evflag) ev_tally_full(i, 0, 0, fpair,
                                    drij(0,l), drij(1,l), drij(2,l));
          //if (evflag) ev_tally(i, tagsj[l], nlocal, newton_pair,
          //                     0, 0, fpair,
          //                     drij(0,l), drij(1,l), drij(2,l));
        }
      }

      // G4/G5: neighbours-1 triplet environments per symmetry function
      else {

        for (int l=0; l < neighbours-1; l++) {

          int numberOfTriplets = arma::size(Riks[l])(1);

          arma::mat dEdRj3(3, numberOfTriplets);
          arma::mat dEdRk3(3, numberOfTriplets); // triplet force for all atoms k

          // calculate forces for all triplets (i,j,k) for this (i,j)
          // fj3 and dEdR3 is passed by reference
          // all triplet forces are summed and stored for j in fj3
          // dEdR3 will contain triplet forces for all k, need 
          // each one seperately for N3L
          // Riks[l], Rjks[l], cosThetas[l] : (1,numberOfTriplets)
          // dEdR3, driks[l]: (3, numberOfTriplets)
          // drij.col(l): (1,3)
          dG4dR(Rij(0,l), Riks[l], Rjks[l], cosThetas[l],
                m_parameters[s][0], m_parameters[s][1], 
                m_parameters[s][2], m_parameters[s][3], 
                dEdRj3, dEdRk3, drij.col(l), driks[l], drjks[l]); 

          // N3L: add 3-body forces for i and k
          double fj3[3];
          double fk3[3];
          for (int m=0; m < numberOfTriplets; m++) {

            // triplet force j
            fj3[0] = dEdG(0,s) * dEdRj3(0,m);
            fj3[1] = dEdG(0,s) * dEdRj3(1,m);
            fj3[2] = dEdG(0,s) * dEdRj3(2,m);

            // triplet force k
            fk3[0] = dEdG(0,s) * dEdRk3(0,m);
            fk3[1] = dEdG(0,s) * dEdRk3(1,m);
            fk3[2] = dEdG(0,s) * dEdRk3(2,m);

            // add both j and k to atom i
            fx3j += fj3[0];
            fy3j += fj3[1];
            fz3j += fj3[2];
            fx3k += fk3[0];
            fy3k += fk3[1];
            fz3k += fk3[2];

            // add to atom j. Not N3L, but becuase
            // every pair (i,j) is counted twice for triplets
            f[tagsj[l]][0] -= fj3[0];
            f[tagsj[l]][1] -= fj3[1];
            f[tagsj[l]][2] -= fj3[2];

            // add to atom k because of the sum k > j
            f[tagsk[l][m]][0] -= fk3[0];
            f[tagsk[l][m]][1] -= fk3[1];
            f[tagsk[l][m]][2] -= fk3[2];

            if (evflag) ev_tally3_nn(i, tagsj[l], tagsk[l][m],
                                     fj3, fk3, 
                                     drij(0,l), drij(1,l), drij(2,l),
                                     driks[l](0,m), driks[l](1,m), driks[l](2,m));
          }
        }
      }
    }

    // update forces
    f[i][0] += fx2 + fx3j + fx3k;
    f[i][1] += fy2 + fy3j + fy3k;
    f[i][2] += fz2 + fz3j + fz3k;
  }*/
  if (vflag_fdotr) virial_fdotr_compute();
  myStep++;
}

/* ---------------------------------------------------------------------- */

void PairNNAngular3::allocate()
{
  allocated = 1;
  int n = atom->ntypes;
  memory->create(setflag,n+1,n+1,"pair:setflag");
  memory->create(cutsq,n+1,n+1,"pair:cutsq"); 
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairNNAngular3::settings(int narg, char **arg)
{
  if (narg != 0) error->all(FLERR,"Illegal pair_style command");
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

void PairNNAngular3::coeff(int narg, char **arg)
{
  if (!allocated) allocate();

  if (narg != 4)
    error->all(FLERR,"Incorrect args for pair coefficients");

  // insure I,J args are * *

  if (strcmp(arg[0],"*") != 0 || strcmp(arg[1],"*") != 0)
    error->all(FLERR,"Incorrect args for pair coefficients");

  // read potential file and initialize potential parameters
  read_file(arg[2]);
  cutoff = force->numeric(FLERR,arg[3]);

  // let lammps know that we have set all parameters
  int n = atom->ntypes;
  int count = 0;
  for (int i = 1; i <= n; i++) {
    for (int j = i; j <= n; j++) {
        setflag[i][j] = 1;
        count++;
    }
  }

  if (count == 0) error->all(FLERR,"Incorrect args for pair coefficients");
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairNNAngular3::init_style()
{
  if (atom->tag_enable == 0)
    error->all(FLERR,"Pair style NN requires atom IDs");
  //if (force->newton_pair == 0)
    //error->all(FLERR,"Pair style NN requires newton pair on");

  // need a full neighbor list
  int irequest = neighbor->request(this);
  neighbor->requests[irequest]->half = 0;
  neighbor->requests[irequest]->full = 1;
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairNNAngular3::init_one(int i, int j)
{
  if (setflag[i][j] == 0) error->all(FLERR,"All pair coeffs are not set");

  return cutoff;
}

/* ---------------------------------------------------------------------- */

void PairNNAngular3::read_file(char *file)
{
  // convert to string 
  std::string trainingDir(file);
  std::string graphFile = trainingDir + "/graph.dat";
  std::cout << "Graph file: " << graphFile << std::endl;
  
  // open graph file
  std::ifstream inputGraph;
  inputGraph.open(graphFile.c_str(), std::ios::in);

  // check if file successfully opened
  if ( !inputGraph.is_open() ) std::cout << "File is not opened" << std::endl;

  // process first line
  std::string activation;
  inputGraph >> m_nLayers >> m_nNodes >> activation >> 
                m_numberOfInputs >> m_numberOfOutputs;
  std::cout << "Layers: "     << m_nLayers         << std::endl;
  std::cout << "Nodes: "      << m_nNodes          << std::endl;
  std::cout << "Activation: " << activation        << std::endl;
  std::cout << "Neighbours: " << m_numberOfInputs  << std::endl;
  std::cout << "Outputs: "    << m_numberOfOutputs << std::endl;

  // set sizes
  m_preActivations.resize(m_nLayers+2);
  m_activations.resize(m_nLayers+2);
  m_derivatives.resize(m_nLayers+2);

  // skip a blank line
  std::string dummyLine;
  std::getline(inputGraph, dummyLine);

  // process file
  // store all weights in a temporary vector
  // that will be reshaped later
  std::vector<arma::mat> weightsTemp = std::vector<arma::mat>();
  for ( std::string line; std::getline(inputGraph, line); ) {

    if ( line.empty() )
        break;

    // store all weights in a vector
    double buffer;                  // have a buffer string
    std::stringstream ss(line);     // insert the string into a stream

    // while there are new weights on current line, add them to vector
    arma::mat matrix(1,m_nNodes);
    int i = 0;
    while ( ss >> buffer ) {
        matrix(0,i) = buffer;
        i++;
    }
    weightsTemp.push_back(matrix);
  }

  // can put all biases in vector directly
  // no need for temporary vector
  for ( std::string line; std::getline(inputGraph, line); ) {

    // store all weights in vector
    double buffer;                  // have a buffer string
    std::stringstream ss(line);     // insert the string into a stream

    // while there are new weights on current line, add them to vector
    arma::mat matrix(1,m_nNodes);
    int i = 0;
    while ( ss >> buffer ) {
        matrix(0,i) = buffer;
        i++;
    }
    m_biases.push_back(matrix);
  }

  // close file
  inputGraph.close();

  // write out all weights and biases
  /*for (const auto i : weightsTemp)
    std::cout << i << std::endl;
  std::cout << std::endl;
  for (const auto i : m_biases)
    std::cout << i << std::endl;*/

  // resize weights and biases matrices to correct shapes
  m_weights.resize(m_nLayers+1);

  // first hidden layer
  int currentRow = 0;
  m_weights[0]  = weightsTemp[currentRow];
  for (int i=0; i < m_numberOfInputs-1; i++) {
    currentRow++;
    m_weights[0] = arma::join_cols(m_weights[0], weightsTemp[currentRow]);
  }

  // following hidden layers
  for (int i=0; i < m_nLayers-1; i++) {
    currentRow++;
    m_weights[i+1] = weightsTemp[currentRow];
    for (int j=1; j < m_nNodes; j++) {
        currentRow++;
        m_weights[i+1] = arma::join_cols(m_weights[i+1], weightsTemp[currentRow]);
    }
  }

  // output layer
  currentRow++;
  arma::mat outputLayer = weightsTemp[currentRow];
  for (int i=0; i < m_numberOfOutputs-1; i++) {
    currentRow++;
    outputLayer = arma::join_cols(outputLayer, weightsTemp[currentRow]);
  }
  m_weights[m_nLayers] = arma::reshape(outputLayer, m_nNodes, m_numberOfOutputs);

  // reshape bias of output node
  m_biases[m_nLayers].shed_cols(1,m_nNodes-1);

  // obtained transposed matrices
  m_weightsTransposed.resize(m_nLayers+1);
  for (int i=0; i < m_weights.size(); i++)
    m_weightsTransposed[i] = m_weights[i].t();

  // write out entire system for comparison
  /*for (const auto i : m_weights)
    std::cout << i << std::endl;

  for (const auto i : m_biases)
    std::cout << i << std::endl;*/

  // read parameters file
  std::string parametersName = trainingDir + "/parameters.dat";

  std::cout << "Parameters file: " << parametersName << std::endl;

  std::ifstream inputParameters;
  inputParameters.open(parametersName.c_str(), std::ios::in);

  // check if file successfully opened
  if ( !inputParameters.is_open() ) std::cout << "File is not opened" << std::endl;

  inputParameters >> m_numberOfSymmFunc;

  std::cout << "Number of symmetry functions: " << m_numberOfSymmFunc << std::endl;

  // skip blank line
  std::getline(inputParameters, dummyLine);

  int i = 0;
  for ( std::string line; std::getline(inputParameters, line); ) {

    if ( line.empty() )
      break;

    double buffer;                  // have a buffer string
    std::stringstream ss(line);     // insert the string into a stream

    // while there are new parameters on current line, add them to matrix
    m_parameters.resize(m_numberOfSymmFunc);  
    while ( ss >> buffer ) {
        m_parameters[i].push_back(buffer);
    }
    i++;
  }
  inputParameters.close();
  std::cout << "File read......" << std::endl;
}
