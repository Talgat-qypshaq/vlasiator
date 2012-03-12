/*
This file is part of Vlasiator.

Copyright 2010, 2011, 2012 Finnish Meteorological Institute

Vlasiator is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 3
as published by the Free Software Foundation.

Vlasiator is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

// ***************************************************************
// On the divergence-free condition in Godunov-type schemes for
// ideal magnetohydrodynamics: the upwind constrained transport method,
// P. Londrillo and L. Del Zanna, J. Comp. Phys., 195, 2004.
// http://dx.doi.org/10.1016/j.jcp.2003.09.016
//
// Reconstructions taken from:
// Efficient, high accuracy ADER-WENO schemes for hydrodynamics and
// divergence-free magnetohydrodynamics, D. S. Balsaraa, T. Rumpfa,
// M. Dumbserb, C.-D. Munzc, J. Comp. Phys, 228, 2480-2516, 2009.
// http://dx.doi.org/10.1016/j.jcp.2008.12.003
// and
// Divergence-free reconstruction of magnetic fields and WENO
// schemes for magnetohydrodynamics, D. S. Balsara, J. Comp. Phys.,
// 228, 5040-5056, 2009.
// http://dx.doi.org/10.1016/j.jcp.2009.03.038
// *****                                                     *****
// *****  NOTATION USED FOR VARIABLES FOLLOWS THE ONES USED  *****
// *****      IN THE ABOVEMENTIONED PUBLICATION(S)           *****
// ***************************************************************

#include <cstdlib>
#include <iostream>
#include <cmath>
#include <vector>
#include <map>
#include <list>
#include <set>

#include "../common.h"
#include "../parameters.h"
#include "../fieldsolver.h"
#include "../priorityqueue.h"
#include "limiters.h"
#include "../project.h"
#include "profile.hpp"

using namespace std;
using namespace fieldsolver;

#include <stdint.h>
typedef uint64_t CellID;

static creal EPS = 1.0e-30;

static set<CellID> ghostCells;
static PriorityQueue<CellID> readyCells;      // Priority queue containing cell IDs that are ready to be computed

static map<CellID,uint> boundaryFlags;        // Boundary status flags for all cells on this process. Here "boundary cell" 
                                              // means that the cell is at the physical boundary of the simulation volume, 
					      // in some cases this condition means that this cell is a "ghost cell". However, 
					      // this is algorithm-dependent, so one must be careful with ghost cell definition.
					      // 
					      // Consider a cell and its immediate neighbours (26 in total), i.e. a 3x3 cube 
					      // of cells at base grid level. Considered cell is at the center of the cube. 
					      // Number the cells with the usual C array numbering, k*9+j*3+i, where i,j,k 
					      // are the cell indices in x,y,z directions. Each existing cell within the 
					      // 3x3 cube has its bit (calculated with C array indexing) flipped to value 1.
					      // The bit 13 is always set to unit value (considered cell always exists).
					      // 
					      // These boundary flags can be used to determine whether a numerical algorithm 
					      // should be applied to a cell, for example, to calculate an edge electric field.
					      // The boundary status can be checked with a single bitwise operation instead of 
					      // N if-statements.
					      // 
					      // Note that this definition works with mesh refinement. The boundary flag 
					      // should only change for a cell if some of its neighbours are deleted or 
					      // created during the simulation.


static uint CALCULATE_DX = 0; /**< Bit mask determining if x-derivatives can be calculated on a cell.*/
static uint CALCULATE_DY = 0; /**< Bit mask determining if y-derivatives can be calculated on a cell.*/
static uint CALCULATE_DZ = 0; /**< Bit mask determining if z-derivatives can be calculated on a cell.*/
static uint CALCULATE_EX = 0; /**< Bit mask determining if edge Ex can be calculated on a cell.*/
static uint CALCULATE_EY = 0; /**< Bit mask determining if edge Ey can be calculated on a cell.*/
static uint CALCULATE_EZ = 0; /**< Bit mask determining if edge Ez can be calculated on a cell.*/
static uint PROPAGATE_BX = 0; /**< Bit mask determining if face Bx is propagated on a cell.*/
static uint PROPAGATE_BY = 0; /**< Bit mask determining if face By is propagated on a cell.*/
static uint PROPAGATE_BZ = 0; /**< Bit mask determining if face Bz is propagated on a cell.*/

static bool fieldsArePropagated = false; /**< If true, E and B fields are propagated by the field solver. 
					  * If false, user has set correct volume-averaged E,B field values to cellParams arrays.
					  * In the latter case field solver should not do anything.*/

// Constants: not needed as such, but if field solver is implemented on GPUs 
// these force CPU to use float accuracy, which in turn helps to compare 
// CPU and GPU results.

const Real HALF    = 0.5;
const Real MINUS   = -1.0;
const Real PLUS    = +1.0;
const Real EIGTH   = 1.0/8.0;
const Real FOURTH  = 1.0/4.0;
const Real SIXTH   = 1.0/6.0;
const Real TWELWTH = 1.0/12.0;
const Real TWO     = 2.0;
const Real ZERO    = 0.0;


void calculateDerivativesSimple(dccrg::Dccrg<SpatialCell>& mpiGrid,const vector<CellID>& localCells);
void calculateUpwindedElectricFieldSimple(dccrg::Dccrg<SpatialCell>& mpiGrid,const vector<CellID>& localCells);


/** Calculate the neighbour number. For the inspected cell the (i,j,k) are (1,1,1). Add or 
 * reduce one from an index to get the "neighbour number" for the neighbour in that direction. 
 * For example, neighbour number for i-1,j-1,k neighbour is calculated with calcNbrNumber(1-1,1-1,1+0).
 * Thus, the cell in question has a "neighbour number" 13.
 * The purpose of this function (and neighbour numbers) is to indicate whether a cell has 
 * existing neighbours on a given direction. The neighbour existence status can be stored in 
 * a single 32bit word and tested with bitwise operations.
 */
inline uchar calcNbrNumber(const uchar& i,const uchar& j,const uchar& k) {return k*9+j*3+i;}

inline uchar calcNbrTypeID(const uchar& i,const uchar& j,const uchar& k) {return k*25+j*5+i;}

Real limiter(creal& left,creal& cent,creal& rght) {
   //const Real limited = minmod(left,cent,rght);
   const Real limited = MClimiter(left,cent,rght);
   //const Real limited = vanLeer(left,cent,rght);

   #ifdef DEBUG_SOLVERS
   if (limited != limited
   || limited * 0 != 0) {
      std::cerr << __FILE__ << ":" << __LINE__
         << " Limiter returned an invalid value " << limited
         << " with left, center, right: " << left << ", " << cent << ", " << rght
         << std::endl;
      abort();
   }
   #endif

   return limited;
}

CellID getNeighbourID(
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	const CellID& cellID,
	const uchar& i,
	const uchar& j,
	const uchar& k
) {
   const std::vector<CellID> neighbors = mpiGrid.get_neighbors_of(cellID, int(i) - 2, int(j) - 2, int(k) - 2);
   if (neighbors.size() == 0) {
      cerr << __FILE__ << ":" << __LINE__
         << " No neighbor for cell " << cellID
         << " at offsets " << int(i) - 2 << ", " << int(j) - 2 << ", " << int(k) - 2
         << endl;
      abort();
   }
   // TODO support spatial refinement
   return neighbors[0];
}

static void calculateBoundaryFlags(
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	const vector<CellID>& localCells
) {
   boundaryFlags.clear();
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      
      // Raise the bit for each existing cell within a 3x3 cube of 
      // spatial cells. This cell sits at the center of the cube.
      uint boundaryFlag = (1 << calcNbrNumber(1,1,1)); // The cell itself exists (bit 13 set to 1)
      
      for (int k=-1; k<2; ++k) for (int j=-1; j<2; ++j) for (int i=-1; i<2; ++i) {
	 if (i == 0 && (j == 0 && k == 0)) continue;
         const CellID nbr = getNeighbourID(mpiGrid, cellID, 2 + i, 2 + j, 2 + k);
	 if (nbr == INVALID_CELLID) continue;
	 boundaryFlag = boundaryFlag | (1 << calcNbrNumber(1+i,1+j,1+k));
      }
      boundaryFlags[cellID] = boundaryFlag;
   }
}

static void calculateDerivatives(
	const CellID& cellID,
	dccrg::Dccrg<SpatialCell>& mpiGrid
) {
   namespace cp = CellParams;
   namespace fs = fieldsolver;
   Real* const array       = mpiGrid[cellID]->derivatives;
   Real* const derivatives = array;
   // Get boundary flag for the cell:
   #ifndef NDEBUG
      map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
      if (it == boundaryFlags.end()) {cerr << "ERROR Could not find boundary flag for cell #" << cellID << endl; exit(1);}
      cuint existingCells = it->second;
   #else
      cuint existingCells = boundaryFlags[cellID];
   #endif
   cuint nonExistingCells = (existingCells ^ numeric_limits<uint>::max());
   
   CellID leftNbrID,rghtNbrID;
   creal* left = NULL;
   creal* cent = mpiGrid[cellID   ]->parameters;   
   #ifdef DEBUG_SOLVERS
   if (cent[cp::RHO] <= 0) {
      std::cerr << __FILE__ << ":" << __LINE__
         << (cent[cp::RHO] < 0 ? " Negative" : " Zero") << " density in spatial cell " << cellID
         << std::endl;
      abort();
   }
   #endif
   
   creal* rght = NULL;
   // Calculate x-derivatives (is not TVD for AMR mesh):
   if ((existingCells & CALCULATE_DX) == CALCULATE_DX) {
      leftNbrID = getNeighbourID(mpiGrid,cellID,2-1,2  ,2  );
      rghtNbrID = getNeighbourID(mpiGrid,cellID,2+1,2  ,2  );
      left = mpiGrid[leftNbrID]->parameters;
      #ifdef DEBUG_SOLVERS
      if (left[cp::RHO] <= 0) {
         std::cerr << __FILE__ << ":" << __LINE__
            << (left[cp::RHO] < 0 ? " Negative" : " Zero") << " density in spatial cell " << leftNbrID
            << std::endl;
         abort();
      }
      #endif
      rght = mpiGrid[rghtNbrID]->parameters;
      #ifdef DEBUG_SOLVERS
      if (rght[cp::RHO] <= 0) {
         std::cerr << __FILE__ << ":" << __LINE__
            << (rght[cp::RHO] < 0 ? " Negative" : " Zero") << " density in spatial cell " << rghtNbrID
            << std::endl;
         abort();
      }
      #endif

      CHECK_FLOAT(left[cp::RHO])
      CHECK_FLOAT(cent[cp::RHO])
      CHECK_FLOAT(rght[cp::RHO])
      array[fs::drhodx] = limiter(left[cp::RHO],cent[cp::RHO],rght[cp::RHO]);
      CHECK_FLOAT(array[fs::drhodx])

      CHECK_FLOAT(left[cp::BY])
      CHECK_FLOAT(cent[cp::BY])
      CHECK_FLOAT(rght[cp::BY])
      array[fs::dBydx]  = limiter(
         left[cp::BY] + left[cp::BYFACEY0],
         cent[cp::BY] + cent[cp::BYFACEY0],
         rght[cp::BY] + rght[cp::BYFACEY0]
      );
      CHECK_FLOAT(array[fs::dBydx])

      CHECK_FLOAT(left[cp::BZ])
      CHECK_FLOAT(cent[cp::BZ])
      CHECK_FLOAT(rght[cp::BZ])
      array[fs::dBzdx]  = limiter(
         left[cp::BZ] + left[cp::BZFACEZ0],
         cent[cp::BZ] + cent[cp::BZFACEZ0],
         rght[cp::BZ] + rght[cp::BZFACEZ0]
      );
      CHECK_FLOAT(array[fs::dBzdx])

      array[fs::dVxdx]  = limiter(left[cp::RHOVX]/left[cp::RHO],cent[cp::RHOVX]/cent[cp::RHO],rght[cp::RHOVX]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVxdx])
      array[fs::dVydx]  = limiter(left[cp::RHOVY]/left[cp::RHO],cent[cp::RHOVY]/cent[cp::RHO],rght[cp::RHOVY]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVydx])
      array[fs::dVzdx]  = limiter(left[cp::RHOVZ]/left[cp::RHO],cent[cp::RHOVZ]/cent[cp::RHO],rght[cp::RHOVZ]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVzdx])
   } else {
      fieldSolverBoundaryCondDerivX(cellID,array,existingCells,nonExistingCells,derivatives,mpiGrid);
   }
   
   // Calculate y-derivatives (is not TVD for AMR mesh):
   if ((existingCells & CALCULATE_DY) == CALCULATE_DY) {
      leftNbrID = getNeighbourID(mpiGrid,cellID,2  ,2-1,2  );
      rghtNbrID = getNeighbourID(mpiGrid,cellID,2  ,2+1,2  );

      left = mpiGrid[leftNbrID]->parameters;
      #ifdef DEBUG_SOLVERS
      if (left[cp::RHO] <= 0) {
         std::cerr << __FILE__ << ":" << __LINE__
            << (left[cp::RHO] < 0 ? " Negative" : " Zero") << " density in spatial cell " << leftNbrID
            << " Zero density in spatial cell " << leftNbrID
            << std::endl;
         abort();
      }
      #endif
      
      rght = mpiGrid[rghtNbrID]->parameters;
      #ifdef DEBUG_SOLVERS
      if (rght[cp::RHO] <= 0) {
         std::cerr << __FILE__ << ":" << __LINE__
            << (rght[cp::RHO] < 0 ? " Negative" : " Zero") << " density in spatial cell " << rghtNbrID
            << std::endl;
         abort();
      }
      #endif

      CHECK_FLOAT(left[cp::RHO])
      CHECK_FLOAT(cent[cp::RHO])
      CHECK_FLOAT(rght[cp::RHO])
      array[fs::drhody] = limiter(left[cp::RHO],cent[cp::RHO],rght[cp::RHO]);
      CHECK_FLOAT(array[fs::drhody])

      CHECK_FLOAT(left[cp::BX])
      CHECK_FLOAT(cent[cp::BX])
      CHECK_FLOAT(rght[cp::BX])
      array[fs::dBxdy]  = limiter(
         left[cp::BX] + left[cp::BXFACEX0],
         cent[cp::BX] + cent[cp::BXFACEX0],
         rght[cp::BX] + rght[cp::BXFACEX0]
      );
      CHECK_FLOAT(array[fs::dBxdy])

      CHECK_FLOAT(left[cp::BZ])
      CHECK_FLOAT(cent[cp::BZ])
      CHECK_FLOAT(rght[cp::BZ])
      array[fs::dBzdy]  = limiter(
         left[cp::BZ] + left[cp::BZFACEZ0],
         cent[cp::BZ] + cent[cp::BZFACEZ0],
         rght[cp::BZ] + rght[cp::BZFACEZ0]
      );
      CHECK_FLOAT(array[fs::dBzdy])

      array[fs::dVxdy]  = limiter(left[cp::RHOVX]/left[cp::RHO],cent[cp::RHOVX]/cent[cp::RHO],rght[cp::RHOVX]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVxdy])
      array[fs::dVydy]  = limiter(left[cp::RHOVY]/left[cp::RHO],cent[cp::RHOVY]/cent[cp::RHO],rght[cp::RHOVY]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVydy])
      array[fs::dVzdy]  = limiter(left[cp::RHOVZ]/left[cp::RHO],cent[cp::RHOVZ]/cent[cp::RHO],rght[cp::RHOVZ]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVzdy])
   } else {
      fieldSolverBoundaryCondDerivY(cellID,array,existingCells,nonExistingCells,derivatives,mpiGrid);
   }
   
   // Calculate z-derivatives (is not TVD for AMR mesh):
   if ((existingCells & CALCULATE_DZ) == CALCULATE_DZ) {
      leftNbrID = getNeighbourID(mpiGrid,cellID,2  ,2  ,2-1);
      rghtNbrID = getNeighbourID(mpiGrid,cellID,2  ,2  ,2+1);
      left = mpiGrid[leftNbrID]->parameters;
      #ifdef DEBUG_SOLVERS
      if (left[cp::RHO] <= 0) {
         std::cerr << __FILE__ << ":" << __LINE__
            << (left[cp::RHO] < 0 ? " Negative" : " Zero") << " density in spatial cell " << leftNbrID
            << std::endl;
         abort();
      }
      #endif
      rght = mpiGrid[rghtNbrID]->parameters;
      #ifdef DEBUG_SOLVERS
      if (rght[cp::RHO] <= 0) {
         std::cerr << __FILE__ << ":" << __LINE__
            << (rght[cp::RHO] < 0 ? " Negative" : " Zero") << " density in spatial cell " << rghtNbrID
            << std::endl;
         abort();
      }
      #endif

      CHECK_FLOAT(left[cp::RHO])
      CHECK_FLOAT(cent[cp::RHO])
      CHECK_FLOAT(rght[cp::RHO])
      array[fs::drhodz] = limiter(left[cp::RHO],cent[cp::RHO],rght[cp::RHO]);
      CHECK_FLOAT(array[fs::drhodz])

      CHECK_FLOAT(left[cp::BX])
      CHECK_FLOAT(cent[cp::BX])
      CHECK_FLOAT(rght[cp::BX])
      array[fs::dBxdz]  = limiter(
         left[cp::BX] + left[cp::BXFACEX0],
         cent[cp::BX] + cent[cp::BXFACEX0],
         rght[cp::BX] + rght[cp::BXFACEX0]
      );
      CHECK_FLOAT(array[fs::dBxdz])

      CHECK_FLOAT(left[cp::BY])
      CHECK_FLOAT(cent[cp::BY])
      CHECK_FLOAT(rght[cp::BY])
      array[fs::dBydz]  = limiter(
         left[cp::BY] + left[cp::BYFACEY0],
         cent[cp::BY] + cent[cp::BYFACEY0],
         rght[cp::BY] + rght[cp::BYFACEY0]
      );
      CHECK_FLOAT(array[fs::dBydz])

      array[fs::dVxdz]  = limiter(left[cp::RHOVX]/left[cp::RHO],cent[cp::RHOVX]/cent[cp::RHO],rght[cp::RHOVX]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVxdz])
      array[fs::dVydz]  = limiter(left[cp::RHOVY]/left[cp::RHO],cent[cp::RHOVY]/cent[cp::RHO],rght[cp::RHOVY]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVydz])
      array[fs::dVzdz]  = limiter(left[cp::RHOVZ]/left[cp::RHO],cent[cp::RHOVZ]/cent[cp::RHO],rght[cp::RHOVZ]/rght[cp::RHO]);
      CHECK_FLOAT(array[fs::dVzdz])
         } else {
      fieldSolverBoundaryCondDerivZ(cellID,array,existingCells,nonExistingCells,derivatives,mpiGrid);
   }
}

template<typename REAL> REAL calculateFastMSspeedYZ(const REAL* cp,const REAL* derivs,const REAL* nbr_cp,const REAL* nbr_derivs,
						    const REAL& By,const REAL& Bz,
						    const REAL& dBydx,const REAL& dBydz,const REAL& dBzdx,const REAL& dBzdy,
						    const REAL& ydir,const REAL& zdir) {
   namespace fs = fieldsolver;
   namespace pc = physicalconstants;

   const REAL A_0  = HALF*(nbr_cp[CellParams::BX] + nbr_cp[CellParams::BXFACEX0] + cp[CellParams::BX] + cp[CellParams::BXFACEX0]);
   const REAL A_X  = (nbr_cp[CellParams::BX] + nbr_cp[CellParams::BXFACEX0]) - (cp[CellParams::BX] + cp[CellParams::BXFACEX0]);
   const REAL A_Y  = nbr_derivs[fs::dBxdy]  + derivs[fs::dBxdy];
   const REAL A_XY = nbr_derivs[fs::dBxdy]  - derivs[fs::dBxdy];
   const REAL A_Z  = nbr_derivs[fs::dBxdz]  + derivs[fs::dBxdz];
   const REAL A_XZ = nbr_derivs[fs::dBxdz]  - derivs[fs::dBxdz];
   
   const REAL Bx2  = (A_0 + ydir*HALF*A_Y + zdir*HALF*A_Z)*(A_0 + ydir*HALF*A_Y + zdir*HALF*A_Z)
     + TWELWTH*(A_X + ydir*HALF*A_XY + zdir*HALF*A_XZ)*(A_X + ydir*HALF*A_XY + zdir*HALF*A_XZ); // OK
   const REAL By2  = (By + zdir*HALF*dBydz)*(By + zdir*HALF*dBydz) + TWELWTH*dBydx*dBydx; // OK
   const REAL Bz2  = (Bz + ydir*HALF*dBzdy)*(Bz + ydir*HALF*dBzdy) + TWELWTH*dBzdx*dBzdx; // OK
   
   const REAL rho = Parameters::m*(cp[CellParams::RHO] + ydir*HALF*derivs[fs::drhody] + zdir*HALF*derivs[fs::drhodz]);
   return sqrt((Bx2+By2+Bz2) / (pc::MU_0 * rho));
}

template<typename REAL> REAL calculateFastMSspeedXZ(const REAL* cp,const REAL* derivs,const REAL* nbr_cp,const REAL* nbr_derivs,
						    const REAL& Bx,const REAL& Bz,
						    const REAL& dBxdy,const REAL& dBxdz,const REAL& dBzdx,const REAL& dBzdy,
						    const REAL& xdir,const REAL& zdir) {
   namespace fs = fieldsolver;
   namespace pc = physicalconstants;
   
   const REAL B_0  = HALF*(nbr_cp[CellParams::BY] + nbr_cp[CellParams::BYFACEY0] + cp[CellParams::BY] + cp[CellParams::BYFACEY0]);
   const REAL B_Y  = (nbr_cp[CellParams::BY] + nbr_cp[CellParams::BYFACEY0]) - (cp[CellParams::BY] + cp[CellParams::BYFACEY0]);
   const REAL B_X  = nbr_derivs[fs::dBydx]  + derivs[fs::dBydx];
   const REAL B_XY = nbr_derivs[fs::dBydx]  - derivs[fs::dBydx];
   const REAL B_Z  = nbr_derivs[fs::dBydz]  + derivs[fs::dBydz];
   const REAL B_YZ = nbr_derivs[fs::dBydz]  - derivs[fs::dBydz];
   
   const REAL By2  = (B_0 + xdir*HALF*B_X + zdir*HALF*B_Z)*(B_0 + xdir*HALF*B_X + zdir*HALF*B_Z)
     + TWELWTH*(B_Y + xdir*HALF*B_XY + zdir*HALF*B_YZ)*(B_Y + xdir*HALF*B_XY + zdir*HALF*B_YZ); // OK
   const REAL Bx2  = (Bx + zdir*HALF*dBxdz)*(Bx + zdir*HALF*dBxdz) + TWELWTH*dBxdy*dBxdy; // OK
   const REAL Bz2  = (Bz + xdir*HALF*dBzdx)*(Bz + xdir*HALF*dBzdx) + TWELWTH*dBzdy*dBzdy; // OK
   
   const REAL rho = Parameters::m*(cp[CellParams::RHO] + xdir*HALF*derivs[fs::drhodx] + zdir*HALF*derivs[fs::drhodz]);
   return sqrt((Bx2+By2+Bz2) / (pc::MU_0 * rho));
}

template<typename REAL> REAL calculateFastMSspeedXY(const REAL* cp,const REAL* derivs,const REAL* nbr_cp,const REAL* nbr_derivs,
						    const REAL& Bx,const REAL& By,
						    const REAL& dBxdy,const REAL& dBxdz,const REAL& dBydx,const REAL& dBydz,
						    const REAL& xdir,const REAL& ydir) {
   namespace fs = fieldsolver;
   namespace pc = physicalconstants;
   
   const REAL C_0  = HALF*(nbr_cp[CellParams::BZ] + nbr_cp[CellParams::BZFACEZ0] + cp[CellParams::BZ] + cp[CellParams::BZFACEZ0]);
   const REAL C_Z  = (nbr_cp[CellParams::BZ] + nbr_cp[CellParams::BZFACEZ0]) - (cp[CellParams::BZ] + cp[CellParams::BZFACEZ0]);
   const REAL C_X  = nbr_derivs[fs::dBzdx]  + derivs[fs::dBzdx];
   const REAL C_XZ = nbr_derivs[fs::dBzdx]  - derivs[fs::dBzdx];
   const REAL C_Y  = nbr_derivs[fs::dBzdy]  + derivs[fs::dBzdy];
   const REAL C_YZ = nbr_derivs[fs::dBzdy]  - derivs[fs::dBzdy];
   
   const REAL Bz2  = (C_0 + xdir*HALF*C_X + ydir*HALF*C_Y)*(C_0 + xdir*HALF*C_X + ydir*HALF*C_Y)
     + TWELWTH*(C_Z + xdir*HALF*C_XZ + ydir*HALF*C_YZ)*(C_Z + xdir*HALF*C_XZ + ydir*HALF*C_YZ);
   const REAL Bx2  = (Bx + ydir*HALF*dBxdy)*(Bx + ydir*HALF*dBxdy) + TWELWTH*dBxdz*dBxdz;
   const REAL By2  = (By + xdir*HALF*dBydx)*(By + xdir*HALF*dBydx) + TWELWTH*dBydz*dBydz;
   
   const REAL rho = Parameters::m*(cp[CellParams::RHO] + xdir*HALF*derivs[fs::drhodx] + ydir*HALF*derivs[fs::drhody]);
   return sqrt((Bx2+By2+Bz2) / (pc::MU_0 * rho));
}

static void calculateEdgeElectricFieldX(
	const CellID& cellID,
	dccrg::Dccrg<SpatialCell>& mpiGrid
) {
   namespace fs = fieldsolver;
   
   // An edge has four neighbouring spatial cells. Calculate
   // electric field in each of the four cells per edge.   
   Real ay_pos,ay_neg;              // Max. characteristic velocities to x-direction
   Real az_pos,az_neg;              // Max. characteristic velocities to y-direction
   Real Vy0,Vz0;                    // Reconstructed V
   Real c_y, c_z;                   // Wave speeds to yz-directions

   // Get read-only pointers to NE,NW,SE,SW states (SW is rw, result is written there):
   const CellID nbr_SE = getNeighbourID(mpiGrid, cellID, 2  , 2-1, 2  );
   const CellID nbr_NE = getNeighbourID(mpiGrid, cellID, 2  , 2-1, 2-1);
   const CellID nbr_NW = getNeighbourID(mpiGrid, cellID, 2  , 2  , 2-1);
   #ifndef NDEBUG
      if (nbr_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE neighbour!" << endl; exit(1);}
      if (nbr_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE neighbour!" << endl; exit(1);}
      if (nbr_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW neighbour!" << endl; exit(1);}
   #endif
   
   Real*  const cp_SW = mpiGrid[cellID]->parameters;
   creal* const cp_SE = mpiGrid[nbr_SE]->parameters;
   creal* const cp_NE = mpiGrid[nbr_NE]->parameters;
   creal* const cp_NW = mpiGrid[nbr_NW]->parameters;
   
   creal* const derivs_SW = mpiGrid[cellID]->derivatives;
   creal* const derivs_SE = mpiGrid[nbr_SE]->derivatives;
   creal* const derivs_NE = mpiGrid[nbr_NE]->derivatives;
   creal* const derivs_NW = mpiGrid[nbr_NW]->derivatives;
   
   creal By_S = cp_SW[CellParams::BY] + cp_SW[CellParams::BYFACEY0];
   creal Bz_W = cp_SW[CellParams::BZ] + cp_SW[CellParams::BZFACEZ0];
   creal Bz_E = cp_SE[CellParams::BZ] + cp_SE[CellParams::BZFACEZ0];
   creal By_N = cp_NW[CellParams::BY] + cp_NW[CellParams::BYFACEY0];
   
   creal dBydx_S = derivs_SW[fs::dBydx];
   creal dBydz_S = derivs_SW[fs::dBydz];
   creal dBzdx_W = derivs_SW[fs::dBzdx];
   creal dBzdy_W = derivs_SW[fs::dBzdy];
   creal dBzdx_E = derivs_SE[fs::dBzdx];
   creal dBzdy_E = derivs_SE[fs::dBzdy];
   creal dBydx_N = derivs_NW[fs::dBydx];
   creal dBydz_N = derivs_NW[fs::dBydz];
   
   // Ex and characteristic speeds on this cell:
   Vy0  = cp_SW[CellParams::RHOVY]/cp_SW[CellParams::RHO];
   Vz0  = cp_SW[CellParams::RHOVZ]/cp_SW[CellParams::RHO];

   // 1st order terms:
   Real Ex_SW = By_S*Vz0 - Bz_W*Vy0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ex_SW += +HALF*((By_S - HALF*dBydz_S)*(-derivs_SW[fs::dVzdy] - derivs_SW[fs::dVzdz]) - dBydz_S*Vz0 + SIXTH*dBydx_S*derivs_SW[fs::dVzdx]);
      Ex_SW += -HALF*((Bz_W - HALF*dBzdy_W)*(-derivs_SW[fs::dVydy] - derivs_SW[fs::dVydz]) - dBzdy_W*Vy0 + SIXTH*dBzdx_W*derivs_SW[fs::dVydx]);
   #endif

   const CellID nbrID_SW      = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2  );
   #ifndef NDEBUG
   if (nbrID_SW == INVALID_CELLID) {cerr << "ERROR: Could not find SW cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_SW     = mpiGrid[nbrID_SW]->parameters;
   creal* const nbr_derivs_SW = mpiGrid[nbrID_SW]->derivatives;
   c_y = calculateFastMSspeedYZ(cp_SW,derivs_SW,nbr_cp_SW,nbr_derivs_SW,By_S,Bz_W,dBydx_S,dBydz_S,dBzdx_W,dBzdy_W,MINUS,MINUS);
   c_z = c_y;
   ay_neg   = max(ZERO,-Vy0 + c_y);
   ay_pos   = max(ZERO,+Vy0 + c_y);
   az_neg   = max(ZERO,-Vz0 + c_z);
   az_pos   = max(ZERO,+Vz0 + c_z);
   
   // Ex and characteristic speeds on j-1 neighbour:
   Vy0  = cp_SE[CellParams::RHOVY]/cp_SE[CellParams::RHO];
   Vz0  = cp_SE[CellParams::RHOVZ]/cp_SE[CellParams::RHO];

   // 1st order terms:
   Real Ex_SE = By_S*Vz0 - Bz_E*Vy0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ex_SE += +HALF*((By_S - HALF*dBydz_S)*(+derivs_SE[fs::dVzdy] - derivs_SE[fs::dVzdz]) - dBydz_S*Vz0 + SIXTH*dBydx_S*derivs_SE[fs::dVzdx]);
      Ex_SE += -HALF*((Bz_E + HALF*dBzdy_E)*(+derivs_SE[fs::dVydy] - derivs_SE[fs::dVydz]) + dBzdy_E*Vy0 + SIXTH*dBzdx_E*derivs_SE[fs::dVydx]);
   #endif
   
   const CellID nbrID_SE      = getNeighbourID(mpiGrid, cellID, 2+1, 2-1, 2  );
   #ifndef NDEBUG
   if (nbrID_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_SE     = mpiGrid[nbrID_SE]->parameters;
   creal* const nbr_derivs_SE = mpiGrid[nbrID_SE]->derivatives;
   c_y = calculateFastMSspeedYZ(cp_SE,derivs_SE,nbr_cp_SE,nbr_derivs_SE,By_S,Bz_E,dBydx_S,dBydz_S,dBzdx_E,dBzdy_E,PLUS,MINUS);
   c_z = c_y;
   ay_neg   = max(ay_neg,-Vy0 + c_y);
   ay_pos   = max(ay_pos,+Vy0 + c_y);
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   
   // Ex and characteristic speeds on k-1 neighbour:
   Vy0  = cp_NW[CellParams::RHOVY]/cp_NW[CellParams::RHO];
   Vz0  = cp_NW[CellParams::RHOVZ]/cp_NW[CellParams::RHO];
   
   // 1st order terms:
   Real Ex_NW    = By_N*Vz0 - Bz_W*Vy0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ex_NW += +HALF*((By_N + HALF*dBydz_N)*(-derivs_NW[fs::dVzdy] + derivs_NW[fs::dVzdz]) + dBydz_N*Vz0 + SIXTH*dBydx_N*derivs_NW[fs::dVzdx]);
      Ex_NW += -HALF*((Bz_W - HALF*dBzdy_W)*(-derivs_NW[fs::dVydy] + derivs_NW[fs::dVydz]) - dBzdy_W*Vy0 + SIXTH*dBzdx_W*derivs_NW[fs::dVydx]);
   #endif
   
   const CellID nbrID_NW      = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2-1);
   #ifndef NDEBUG
   if (nbrID_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_NW     = mpiGrid[nbrID_NW]->parameters;
   creal* const nbr_derivs_NW = mpiGrid[nbrID_NW]->derivatives;
   c_y = calculateFastMSspeedYZ(cp_NW,derivs_NW,nbr_cp_NW,nbr_derivs_NW,By_N,Bz_W,dBydx_N,dBydz_N,dBzdx_W,dBzdy_W,MINUS,PLUS);
   c_z = c_y;
   ay_neg   = max(ay_neg,-Vy0 + c_y);
   ay_pos   = max(ay_pos,+Vy0 + c_y);
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   
   // Ex and characteristic speeds on j-1,k-1 neighbour:
   Vy0 = cp_NE[CellParams::RHOVY]/cp_NE[CellParams::RHO];
   Vz0 = cp_NE[CellParams::RHOVZ]/cp_NE[CellParams::RHO];
   
   // 1st order terms:
   Real Ex_NE    = By_N*Vz0 - Bz_E*Vy0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ex_NE += +HALF*((By_N + HALF*dBydz_N)*(+derivs_NE[fs::dVzdy] + derivs_NE[fs::dVzdz]) + dBydz_N*Vz0 + SIXTH*dBydx_N*derivs_NE[fs::dVzdx]);
      Ex_NE += -HALF*((Bz_E + HALF*dBzdy_E)*(+derivs_NE[fs::dVydy] + derivs_NE[fs::dVydz]) + dBzdy_E*Vy0 + SIXTH*dBzdx_E*derivs_NE[fs::dVydx]);
   #endif
   
   const CellID nbrID_NE      = getNeighbourID(mpiGrid, cellID, 2+1, 2-1, 2-1);
   #ifndef NDEBUG
   if (nbrID_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_NE     = mpiGrid[nbrID_NE]->parameters;
   creal* const nbr_derivs_NE = mpiGrid[nbrID_NE]->derivatives;
   c_y = calculateFastMSspeedYZ(cp_NE,derivs_NE,nbr_cp_NE,nbr_derivs_NE,By_N,Bz_E,dBydx_N,dBydz_N,dBzdx_E,dBzdy_E,PLUS,PLUS);
   c_z = c_y;
   ay_neg   = max(ay_neg,-Vy0 + c_y);
   ay_pos   = max(ay_pos,+Vy0 + c_y);
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   
   // Calculate properly upwinded edge-averaged Ex:
   cp_SW[CellParams::EX]  = ay_pos*az_pos*Ex_NE + ay_pos*az_neg*Ex_SE + ay_neg*az_pos*Ex_NW + ay_neg*az_neg*Ex_SW;
   cp_SW[CellParams::EX] /= ((ay_pos+ay_neg)*(az_pos+az_neg)+EPS);
   #ifdef FS_1ST_ORDER
      // 1st order diffusive terms:
      cp_SW[CellParams::EX] -= az_pos*az_neg/(az_pos+az_neg+EPS)*(By_S-By_N);
      cp_SW[CellParams::EX] += ay_pos*ay_neg/(ay_pos+ay_neg+EPS)*(Bz_W-Bz_E);
   #else
      // 2nd order diffusive terms
      cp_SW[CellParams::EX] -= az_pos*az_neg/(az_pos+az_neg+EPS)*((By_S-HALF*dBydz_S) - (By_N+HALF*dBydz_N));
      cp_SW[CellParams::EX] += ay_pos*ay_neg/(ay_pos+ay_neg+EPS)*((Bz_W-HALF*dBzdy_W) - (Bz_E+HALF*dBzdy_E));
   #endif
}

static void calculateEdgeElectricFieldY(
	const CellID& cellID,
	dccrg::Dccrg<SpatialCell>& mpiGrid
) {
   // An edge has four neighbouring spatial cells. Calculate
   // electric field in each of the four cells per edge. 
   namespace fs = fieldsolver;
   
   Real ax_pos,ax_neg;              // Max. characteristic velocities to x-direction
   Real az_pos,az_neg;              // Max. characteristic velocities to y-direction
   Real Vx0,Vz0;                    // Reconstructed V
   Real c_x,c_z;                    // Wave speeds to xz-directions
   
   // Get read-only pointers to NE,NW,SE,SW states (SW is rw, result is written there):
   const CellID nbr_SE = getNeighbourID(mpiGrid, cellID, 2  , 2  , 2-1);
   const CellID nbr_NW = getNeighbourID(mpiGrid, cellID, 2-1, 2  , 2  );
   const CellID nbr_NE = getNeighbourID(mpiGrid, cellID, 2-1, 2  , 2-1);
   #ifndef NDEBUG
      if (nbr_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE neighbour!" << endl; exit(1);}
      if (nbr_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE neighbour!" << endl; exit(1);}
      if (nbr_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW neighbour!" << endl; exit(1);}
   #endif
   
   Real* const  cp_SW = mpiGrid[cellID]->parameters;
   creal* const cp_SE = mpiGrid[nbr_SE]->parameters;
   creal* const cp_NE = mpiGrid[nbr_NE]->parameters;
   creal* const cp_NW = mpiGrid[nbr_NW]->parameters;
   
   creal* const derivs_SW = mpiGrid[cellID]->derivatives;
   creal* const derivs_SE = mpiGrid[nbr_SE]->derivatives;
   creal* const derivs_NE = mpiGrid[nbr_NE]->derivatives;
   creal* const derivs_NW = mpiGrid[nbr_NW]->derivatives;
   
   // Fetch required plasma parameters:
   creal Bz_S = cp_SW[CellParams::BZ] + cp_SW[CellParams::BZFACEZ0];
   creal Bx_W = cp_SW[CellParams::BX] + cp_SW[CellParams::BXFACEX0];
   creal Bx_E = cp_SE[CellParams::BX] + cp_SE[CellParams::BXFACEX0];
   creal Bz_N = cp_NW[CellParams::BZ] + cp_NW[CellParams::BZFACEZ0];
   
   creal dBxdy_W = derivs_SW[fs::dBxdy];
   creal dBxdz_W = derivs_SW[fs::dBxdz];
   creal dBzdx_S = derivs_SW[fs::dBzdx];
   creal dBzdy_S = derivs_SW[fs::dBzdy];
   creal dBxdy_E = derivs_SE[fs::dBxdy];
   creal dBxdz_E = derivs_SE[fs::dBxdz];
   creal dBzdx_N = derivs_NW[fs::dBzdx];
   creal dBzdy_N = derivs_NW[fs::dBzdy];
   
   // Ey and characteristic speeds on this cell:
   Vx0  = cp_SW[CellParams::RHOVX]/cp_SW[CellParams::RHO];
   Vz0  = cp_SW[CellParams::RHOVZ]/cp_SW[CellParams::RHO];
   
   // 1st order terms:
   Real Ey_SW  = Bz_S*Vx0 - Bx_W*Vz0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms
      Ey_SW += +HALF*((Bz_S - HALF*dBzdx_S)*(-derivs_SW[fs::dVxdx] - derivs_SW[fs::dVxdz]) - dBzdx_S*Vx0 + SIXTH*dBzdy_S*derivs_SW[fs::dVxdy]);
      Ey_SW += -HALF*((Bx_W - HALF*dBxdz_W)*(-derivs_SW[fs::dVzdx] - derivs_SW[fs::dVzdz]) - dBxdz_W*Vz0 + SIXTH*dBxdy_W*derivs_SW[fs::dVzdy]);
   #endif
   
   const CellID nbrID_SW      = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2  );
   #ifndef NDEBUG
   if (nbrID_SW == INVALID_CELLID) {cerr << "ERROR: Could not find SW cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_SW     = mpiGrid[nbrID_SW]->parameters;
   creal* const nbr_derivs_SW = mpiGrid[nbrID_SW]->derivatives;
   c_z = calculateFastMSspeedXZ(cp_SW,derivs_SW,nbr_cp_SW,nbr_derivs_SW,Bx_W,Bz_S,dBxdy_W,dBxdz_W,dBzdx_S,dBzdy_S,MINUS,MINUS);
   c_x = c_z;
   az_neg   = max(ZERO,-Vz0 + c_z);
   az_pos   = max(ZERO,+Vz0 + c_z);
   ax_neg   = max(ZERO,-Vx0 + c_x);
   ax_pos   = max(ZERO,+Vx0 + c_x);
   
   // Ey and characteristic speeds on k-1 neighbour:
   Vx0  = cp_SE[CellParams::RHOVX]/cp_SE[CellParams::RHO];
   Vz0  = cp_SE[CellParams::RHOVZ]/cp_SE[CellParams::RHO];

   // 1st order terms:
   Real Ey_SE    = Bz_S*Vx0 - Bx_E*Vz0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ey_SE += +HALF*((Bz_S - HALF*dBzdx_S)*(-derivs_SE[fs::dVxdx] + derivs_SE[fs::dVxdz]) - dBzdx_S*Vx0 + SIXTH*dBzdy_S*derivs_SE[fs::dVxdy]);
      Ey_SE += -HALF*((Bx_E + HALF*dBxdz_E)*(-derivs_SE[fs::dVzdx] + derivs_SE[fs::dVzdz]) + dBxdz_E*Vz0 + SIXTH*dBxdy_E*derivs_SE[fs::dVzdy]);
   #endif

   const CellID nbrID_SE      = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2-1);
   #ifndef NDEBUG
   if (nbrID_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_SE     = mpiGrid[nbrID_SE]->parameters;
   creal* const nbr_derivs_SE = mpiGrid[nbrID_SE]->derivatives;
   c_z = calculateFastMSspeedXZ(cp_SE,derivs_SE,nbr_cp_SE,nbr_derivs_SE,Bx_E,Bz_S,dBxdy_E,dBxdz_E,dBzdx_S,dBzdy_S,MINUS,PLUS);
   c_x = c_z;
   az_neg   = max(az_neg,-Vz0 - c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   ax_neg   = max(ax_neg,-Vx0 - c_x);
   ax_pos   = max(ax_pos,+Vx0 + c_x);
   
   // Ey and characteristic speeds on i-1 neighbour:
   Vz0  = cp_NW[CellParams::RHOVZ]/cp_NW[CellParams::RHO];
   Vx0  = cp_NW[CellParams::RHOVX]/cp_NW[CellParams::RHO];
   
   // 1st order terms:
   Real Ey_NW    = Bz_N*Vx0 - Bx_W*Vz0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ey_NW += +HALF*((Bz_N + HALF*dBzdx_N)*(+derivs_NW[fs::dVxdx] - derivs_NW[fs::dVxdz]) + dBzdx_N*Vx0 + SIXTH*dBzdy_N*derivs_NW[fs::dVxdy]);
      Ey_NW += -HALF*((Bx_W - HALF*dBxdz_W)*(+derivs_NW[fs::dVzdx] - derivs_NW[fs::dVzdz]) - dBxdz_W*Vz0 + SIXTH*dBxdy_W*derivs_NW[fs::dVzdy]);
   #endif
   
   const CellID nbrID_NW      = getNeighbourID(mpiGrid, cellID, 2-1, 2+1, 2  );
   #ifndef NDEBUG
   if (nbrID_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_NW     = mpiGrid[nbrID_NW]->parameters;
   creal* const nbr_derivs_NW = mpiGrid[nbrID_NW]->derivatives;
   c_z = calculateFastMSspeedXZ(cp_NW,derivs_NW,nbr_cp_NW,nbr_derivs_NW,Bx_W,Bz_N,dBxdy_W,dBxdz_W,dBzdx_N,dBzdy_N,PLUS,MINUS);
   c_x = c_z;
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   ax_neg   = max(ax_neg,-Vx0 + c_x);
   ax_pos   = max(ax_pos,+Vx0 + c_x);
   
   // Ey and characteristic speeds on i-1,k-1 neighbour:
   Vz0 = cp_NE[CellParams::RHOVZ]/cp_NE[CellParams::RHO];
   Vx0 = cp_NE[CellParams::RHOVX]/cp_NE[CellParams::RHO];

   // 1st order terms:
   Real Ey_NE    = Bz_N*Vx0 - Bx_E*Vz0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ey_NE += +HALF*((Bz_N + HALF*dBzdx_N)*(+derivs_NE[fs::dVxdx] + derivs_NE[fs::dVxdz]) + dBzdx_N*Vx0 + SIXTH*dBzdy_N*derivs_NE[fs::dVxdy]);
      Ey_NE += -HALF*((Bx_E + HALF*dBxdz_E)*(+derivs_NE[fs::dVzdx] + derivs_NE[fs::dVzdz]) + dBxdz_E*Vz0 + SIXTH*dBxdy_E*derivs_NE[fs::dVzdy]);
   #endif
   
   const CellID nbrID_NE      = getNeighbourID(mpiGrid, cellID, 2-1, 2+1, 2-1);
   #ifndef NDEBUG
   if (nbrID_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_NE     = mpiGrid[nbrID_NE]->parameters;
   creal* const nbr_derivs_NE = mpiGrid[nbrID_NE]->derivatives;
   c_z = calculateFastMSspeedXZ(cp_NE,derivs_NE,nbr_cp_NE,nbr_derivs_NE,Bx_E,Bz_N,dBxdy_E,dBxdz_E,dBzdx_N,dBzdy_N,PLUS,PLUS);
   c_x = c_z;
   az_neg   = max(az_neg,-Vz0 + c_z);
   az_pos   = max(az_pos,+Vz0 + c_z);
   ax_neg   = max(ax_neg,-Vx0 + c_x);
   ax_pos   = max(ax_pos,+Vx0 + c_x);
   
   // Calculate properly upwinded edge-averaged Ey:
   cp_SW[CellParams::EY]  = az_pos*ax_pos*Ey_NE + az_pos*ax_neg*Ey_SE + az_neg*ax_pos*Ey_NW + az_neg*ax_neg*Ey_SW;
   cp_SW[CellParams::EY] /= ((az_pos+az_neg)*(ax_pos+ax_neg)+EPS);
   #ifdef FS_1ST_ORDER
      cp_SW[CellParams::EY] -= ax_pos*ax_neg/(ax_pos+ax_neg+EPS)*(Bz_S-Bz_N);
      cp_SW[CellParams::EY] += az_pos*az_neg/(az_pos+az_neg+EPS)*(Bx_W-Bx_E);
   #else
      cp_SW[CellParams::EY] -= ax_pos*ax_neg/(ax_pos+ax_neg+EPS)*((Bz_S-HALF*dBzdx_S) - (Bz_N+HALF*dBzdx_N));
      cp_SW[CellParams::EY] += az_pos*az_neg/(az_pos+az_neg+EPS)*((Bx_W-HALF*dBxdz_W) - (Bx_E+HALF*dBxdz_E));
   #endif
}

static void calculateEdgeElectricFieldZ(
	const CellID& cellID,
	dccrg::Dccrg<SpatialCell>& mpiGrid
) {
   namespace fs = fieldsolver;
   namespace pc = physicalconstants;
   
   // An edge has four neighbouring spatial cells. Calculate 
   // electric field in each of the four cells per edge.
   Real ax_pos,ax_neg;              // Max. characteristic velocities to x-direction
   Real ay_pos,ay_neg;              // Max. characteristic velocities to y-direction
   Real Vx0,Vy0;                    // Reconstructed V
   Real c_x,c_y;                    // Characteristic speeds to xy-directions

   // Get read-only pointers to NE,NW,SE,SW states (SW is rw, result is written there):
   const CellID nbr_SE = getNeighbourID(mpiGrid, cellID, 2-1, 2  , 2  );
   const CellID nbr_NE = getNeighbourID(mpiGrid, cellID, 2-1, 2-1, 2  );
   const CellID nbr_NW = getNeighbourID(mpiGrid, cellID, 2  , 2-1, 2  );
   #ifndef NDEBUG
      if (nbr_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE neighbour!" << endl; exit(1);}
      if (nbr_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE neighbour!" << endl; exit(1);}
      if (nbr_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW neighbour!" << endl; exit(1);}
   #endif
   
   Real* const cp_SW  = mpiGrid[cellID]->parameters;
   creal* const cp_SE = mpiGrid[nbr_SE]->parameters;
   creal* const cp_NE = mpiGrid[nbr_NE]->parameters;
   creal* const cp_NW = mpiGrid[nbr_NW]->parameters;
   
   creal* const derivs_SW = mpiGrid[cellID]->derivatives;
   creal* const derivs_SE = mpiGrid[nbr_SE]->derivatives;
   creal* const derivs_NE = mpiGrid[nbr_NE]->derivatives;
   creal* const derivs_NW = mpiGrid[nbr_NW]->derivatives;
   
   // Fetch needed plasma parameters/derivatives from the four cells:
   creal Bx_S    = cp_SW[CellParams::BX] + cp_SW[CellParams::BXFACEX0];
   creal By_W    = cp_SW[CellParams::BY] + cp_SW[CellParams::BYFACEY0];
   creal By_E    = cp_SE[CellParams::BY] + cp_SE[CellParams::BYFACEY0];
   creal Bx_N    = cp_NW[CellParams::BX] + cp_NW[CellParams::BXFACEX0];
   creal dBxdy_S = derivs_SW[fs::dBxdy];
   creal dBxdz_S = derivs_SW[fs::dBxdz];
   creal dBydx_W = derivs_SW[fs::dBydx];
   creal dBydz_W = derivs_SW[fs::dBydz];
   creal dBydx_E = derivs_SE[fs::dBydx];
   creal dBydz_E = derivs_SE[fs::dBydz];
   creal dBxdy_N = derivs_NW[fs::dBxdy];
   creal dBxdz_N = derivs_NW[fs::dBxdz];
   
   // Ez and characteristic speeds on SW cell:
   Vx0  = cp_SW[CellParams::RHOVX]/cp_SW[CellParams::RHO];
   Vy0  = cp_SW[CellParams::RHOVY]/cp_SW[CellParams::RHO];

   // 1st order terms:
   Real Ez_SW = Bx_S*Vy0 - By_W*Vx0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ez_SW  += +HALF*((Bx_S - HALF*dBxdy_S)*(-derivs_SW[fs::dVydx] - derivs_SW[fs::dVydy]) - dBxdy_S*Vy0 + SIXTH*dBxdz_S*derivs_SW[fs::dVydz]);
      Ez_SW  += -HALF*((By_W - HALF*dBydx_W)*(-derivs_SW[fs::dVxdx] - derivs_SW[fs::dVxdy]) - dBydx_W*Vx0 + SIXTH*dBydz_W*derivs_SW[fs::dVxdz]);
   #endif

   // Calculate maximum wave speed (fast magnetosonic speed) on SW cell. In order 
   // to get Alfven speed we need to calculate some reconstruction coeff. for Bz:
   const CellID nbrID_SW      = getNeighbourID(mpiGrid, cellID, 2  , 2  , 2+1);
   #ifndef NDEBUG
   if (nbrID_SW == INVALID_CELLID) {cerr << "ERROR: Could not find SW cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_SW     = mpiGrid[nbrID_SW]->parameters;
   creal* const nbr_derivs_SW = mpiGrid[nbrID_SW]->derivatives;
   c_x = calculateFastMSspeedXY(cp_SW,derivs_SW,nbr_cp_SW,nbr_derivs_SW,Bx_S,By_W,dBxdy_S,dBxdz_S,dBydx_W,dBydz_W,MINUS,MINUS);
   c_y = c_x;
   ax_neg   = max(ZERO,-Vx0 + c_x);
   ax_pos   = max(ZERO,+Vx0 + c_x);
   ay_neg   = max(ZERO,-Vy0 + c_y);
   ay_pos   = max(ZERO,+Vy0 + c_y);
   
   // Ez and characteristic speeds on SE (i-1) cell:
   Vx0  = cp_SE[CellParams::RHOVX]/cp_SE[CellParams::RHO];
   Vy0  = cp_SE[CellParams::RHOVY]/cp_SE[CellParams::RHO];
   
   // 1st order terms:
   Real Ez_SE = Bx_S*Vy0 - By_E*Vx0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ez_SE  += +HALF*((Bx_S - HALF*dBxdy_S)*(+derivs_SE[fs::dVydx] - derivs_SE[fs::dVydy]) - dBxdy_S*Vy0 + SIXTH*dBxdz_S*derivs_SE[fs::dVydz]);
      Ez_SE  += -HALF*((By_E + HALF*dBydx_E)*(+derivs_SE[fs::dVxdx] - derivs_SE[fs::dVxdy]) + dBydx_E*Vx0 + SIXTH*dBydz_E*derivs_SE[fs::dVxdz]);
   #endif

   const CellID nbrID_SE      = getNeighbourID(mpiGrid, cellID, 2-1, 2  , 2+1);
   #ifndef NDEBUG
   if (nbrID_SE == INVALID_CELLID) {cerr << "ERROR: Could not find SE cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_SE     = mpiGrid[nbrID_SE]->parameters;
   creal* const nbr_derivs_SE = mpiGrid[nbrID_SE]->derivatives;
   c_x = calculateFastMSspeedXY(cp_SE,derivs_SE,nbr_cp_SE,nbr_derivs_SE,Bx_S,By_E,dBxdy_S,dBxdz_S,dBydx_E,dBydz_E,PLUS,MINUS);
   c_y = c_x;   
   ax_neg = max(ax_neg,-Vx0 + c_x);
   ax_pos = max(ax_pos,+Vx0 + c_x);
   ay_neg = max(ay_neg,-Vy0 + c_y);
   ay_pos = max(ay_pos,+Vy0 + c_y);

   // Ez and characteristic speeds on NW (j-1) cell:
   Vx0  = cp_NW[CellParams::RHOVX]/cp_NW[CellParams::RHO];
   Vy0  = cp_NW[CellParams::RHOVY]/cp_NW[CellParams::RHO];
   
   // 1st order terms:
   Real Ez_NW = Bx_N*Vy0 - By_W*Vx0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ez_NW  += +HALF*((Bx_N + HALF*dBxdy_N)*(-derivs_NW[fs::dVydx] + derivs_NW[fs::dVydy]) + dBxdy_N*Vy0 + SIXTH*dBxdz_N*derivs_NW[fs::dVydz]);
      Ez_NW  += -HALF*((By_W - HALF*dBydx_W)*(-derivs_NW[fs::dVxdx] + derivs_NW[fs::dVxdy]) - dBydx_W*Vx0 + SIXTH*dBydz_W*derivs_NW[fs::dVxdz]);
   #endif

   const CellID nbrID_NW      = getNeighbourID(mpiGrid, cellID, 2  , 2-1, 2+1);
   #ifndef NDEBUG
   if (nbrID_NW == INVALID_CELLID) {cerr << "ERROR: Could not find NW cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_NW     = mpiGrid[nbrID_NW]->parameters;
   creal* const nbr_derivs_NW = mpiGrid[nbrID_NW]->derivatives;
   c_x = calculateFastMSspeedXY(cp_NW,derivs_NW,nbr_cp_NW,nbr_derivs_NW,Bx_N,By_W,dBxdy_N,dBxdz_N,dBydx_W,dBydz_W,MINUS,PLUS);
   c_y = c_x;
   ax_neg = max(ax_neg,-Vx0 + c_x); 
   ax_pos = max(ax_pos,+Vx0 + c_x);
   ay_neg = max(ay_neg,-Vy0 + c_y);
   ay_pos = max(ay_pos,+Vy0 + c_y);
   
   // Ez and characteristic speeds on NE (i-1,j-1) cell:
   Vx0  = cp_NE[CellParams::RHOVX]/cp_NE[CellParams::RHO];
   Vy0  = cp_NE[CellParams::RHOVY]/cp_NE[CellParams::RHO];
   
   // 1st order terms:
   Real Ez_NE = Bx_N*Vy0 - By_E*Vx0;
   #ifndef FS_1ST_ORDER
      // 2nd order terms:
      Ez_NE  += +HALF*((Bx_N + HALF*dBxdy_N)*(+derivs_NE[fs::dVydx] + derivs_NE[fs::dVydy]) + dBxdy_N*Vy0 + SIXTH*dBxdz_N*derivs_NE[fs::dVydz]);
      Ez_NE  += -HALF*((By_E + HALF*dBydx_E)*(+derivs_NE[fs::dVxdx] + derivs_NE[fs::dVxdy]) + dBydx_E*Vx0 + SIXTH*dBydz_E*derivs_NE[fs::dVxdz]);
   #endif
   
   const CellID nbrID_NE      = getNeighbourID(mpiGrid, cellID, 2-1, 2-1, 2+1);
   #ifndef NDEBUG
   if (nbrID_NE == INVALID_CELLID) {cerr << "ERROR: Could not find NE cell!" << endl; exit(1);}
   #endif
   creal* const nbr_cp_NE     = mpiGrid[nbrID_NE]->parameters;
   creal* const nbr_derivs_NE = mpiGrid[nbrID_NE]->derivatives;
   c_x = calculateFastMSspeedXY(cp_NE,derivs_NE,nbr_cp_NE,nbr_derivs_NE,Bx_N,By_E,dBxdy_N,dBxdz_N,dBydx_E,dBydz_E,PLUS,PLUS);
   c_y = c_x;
   ax_neg = max(ax_neg,-Vx0 + c_x);
   ax_pos = max(ax_pos,+Vx0 + c_x);
   ay_neg = max(ay_neg,-Vy0 + c_y);
   ay_pos = max(ay_pos,+Vy0 + c_y);
   
   // Calculate properly upwinded edge-averaged Ez:
   cp_SW[CellParams::EZ] = ax_pos*ay_pos*Ez_NE + ax_pos*ay_neg*Ez_SE + ax_neg*ay_pos*Ez_NW + ax_neg*ay_neg*Ez_SW;
   CHECK_FLOAT(cp_SW[CellParams::EZ])
   cp_SW[CellParams::EZ] /= ((ax_pos+ax_neg)*(ay_pos+ay_neg)+EPS);
   CHECK_FLOAT(cp_SW[CellParams::EZ])
   #ifdef FS_1ST_ORDER
      cp_SW[CellParams::EZ] -= ay_pos*ay_neg/(ay_pos+ay_neg+EPS)*(Bx_S-Bx_N);
      CHECK_FLOAT(cp_SW[CellParams::EZ])
      cp_SW[CellParams::EZ] += ax_pos*ax_neg/(ax_pos+ax_neg+EPS)*(By_W-By_E);
      CHECK_FLOAT(cp_SW[CellParams::EZ])
   #else
      cp_SW[CellParams::EZ] -= ay_pos*ay_neg/(ay_pos+ay_neg+EPS)*((Bx_S-HALF*dBxdy_S) - (Bx_N+HALF*dBxdy_N));
      CHECK_FLOAT(cp_SW[CellParams::EZ])
      cp_SW[CellParams::EZ] += ax_pos*ax_neg/(ax_pos+ax_neg+EPS)*((By_W-HALF*dBydx_W) - (By_E+HALF*dBydx_E));
      CHECK_FLOAT(cp_SW[CellParams::EZ])
   #endif
}

static void propagateMagneticField(
	const CellID& cellID,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	creal& dt
) {
   CellID nbrID;
   Real* const cp0 = mpiGrid[cellID]->parameters;
   creal* cp1;
   creal* cp2;
   creal dx = cp0[CellParams::DX];
   CHECK_FLOAT(dx)
   creal dy = cp0[CellParams::DY];
   CHECK_FLOAT(dy)
   creal dz = cp0[CellParams::DZ];
   CHECK_FLOAT(dz)
   
   #ifndef NDEBUG
      map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
      if (it == boundaryFlags.end()) {cerr << "Could not find boundary flags for cell #" << cellID << endl; exit(1);}
      cuint boundaryFlag = it->second;
   #else
      cuint boundaryFlag = boundaryFlags[cellID];
   #endif
   
   // Propagate face-averaged Bx:
   if ((boundaryFlag & PROPAGATE_BX) == PROPAGATE_BX) {
      nbrID = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2  );
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif

      if (mpiGrid[nbrID] == NULL) {
         std::cerr << __FILE__ << ":" << __LINE__
            << " No data for cell " << nbrID
            << " while solving cell " << cellID
            << " in positive y direction"
            << std::endl;
         abort();
      }
      cp1 = mpiGrid[nbrID]->parameters;

      nbrID = getNeighbourID(mpiGrid, cellID, 2  , 2  , 2+1);
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif

      if (mpiGrid[nbrID] == NULL) {
         std::cerr << __FILE__ << ":" << __LINE__
            << " No data for cell " << nbrID
            << " while solving cell " << cellID
            << " in positive z direction"
            << std::endl;
         abort();
      }
      cp2 = mpiGrid[nbrID]->parameters;

      CHECK_FLOAT(cp0[CellParams::EY])
      CHECK_FLOAT(cp0[CellParams::EZ])
      CHECK_FLOAT(cp1[CellParams::EZ])
      CHECK_FLOAT(cp2[CellParams::EY])
      cp0[CellParams::BX] += dt/dz*(cp2[CellParams::EY] - cp0[CellParams::EY]) + dt/dy*(cp0[CellParams::EZ] - cp1[CellParams::EZ]);
      CHECK_FLOAT(cp0[CellParams::BX])
   }
   
   // Propagate face-averaged By:
   if ((boundaryFlag & PROPAGATE_BY) == PROPAGATE_BY) {
      nbrID = getNeighbourID(mpiGrid, cellID, 2  , 2  , 2+1);
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif

      if (mpiGrid[nbrID] == NULL) {
         std::cerr << __FILE__ << ":" << __LINE__
            << " No data for cell " << nbrID
            << " while solving cell " << cellID
            << " in positive z direction"
            << std::endl;
         abort();
      }
      cp1 = mpiGrid[nbrID]->parameters;

      nbrID = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2  );
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif

      if (mpiGrid[nbrID] == NULL) {
         std::cerr << __FILE__ << ":" << __LINE__
            << " No data for cell " << nbrID
            << " while solving cell " << cellID
            << " in positive x direction"
            << std::endl;
         abort();
      }
      cp2 = mpiGrid[nbrID]->parameters;

      CHECK_FLOAT(cp0[CellParams::EZ])
      CHECK_FLOAT(cp0[CellParams::EX])
      CHECK_FLOAT(cp1[CellParams::EX])
      CHECK_FLOAT(cp2[CellParams::EZ])
      cp0[CellParams::BY] += dt/dx*(cp2[CellParams::EZ] - cp0[CellParams::EZ]) + dt/dz*(cp0[CellParams::EX] - cp1[CellParams::EX]);
      CHECK_FLOAT(cp0[CellParams::BY])
   }
      
   // Propagate face-averaged Bz:
   if ((boundaryFlag & PROPAGATE_BZ) == PROPAGATE_BZ) {
      nbrID = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2  );
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif

      if (mpiGrid[nbrID] == NULL) {
         std::cerr << __FILE__ << ":" << __LINE__
            << " No data for cell " << nbrID
            << " while solving cell " << cellID
            << " in positive x direction"
            << std::endl;
         abort();
      }
      cp1 = mpiGrid[nbrID]->parameters;

      nbrID = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2  );
      #ifndef NDEBUG
         if (nbrID == INVALID_CELLID) {cerr << "Failed to get nbr pointer!" << endl; exit(1);}
      #endif

      if (mpiGrid[nbrID] == NULL) {
         std::cerr << __FILE__ << ":" << __LINE__
            << " No data for cell " << nbrID
            << " while solving cell " << cellID
            << " in positive y direction"
            << std::endl;
         abort();
      }
      cp2 = mpiGrid[nbrID]->parameters;

      CHECK_FLOAT(cp0[CellParams::EX])
      CHECK_FLOAT(cp0[CellParams::EY])
      CHECK_FLOAT(cp1[CellParams::EY])
      CHECK_FLOAT(cp2[CellParams::EX])
      cp0[CellParams::BZ] += dt/dy*(cp2[CellParams::EX] - cp0[CellParams::EX]) + dt/dx*(cp0[CellParams::EY] - cp1[CellParams::EY]);
      CHECK_FLOAT(cp0[CellParams::BZ])
   }
}

bool initializeFieldPropagatorAfterRebalance(
	dccrg::Dccrg<SpatialCell>& mpiGrid,
        bool propagateFields
) {
   
   vector<uint64_t> localCells = mpiGrid.get_cells();

   calculateBoundaryFlags(mpiGrid,localCells);

   //need this when computing magentic field later on
   SpatialCell::set_mpi_transfer_type(Transfer::CELL_E);
   int timer=profile::initializeTimer("Communicate electric fields","MPI","Wait");
   profile::start(timer);
   mpiGrid.update_remote_neighbor_data();
   profile::stop(timer);
   
   return true;
}

bool initializeFieldPropagator(
	dccrg::Dccrg<SpatialCell>& mpiGrid,
        bool propagateFields
) {
   fieldsArePropagated = propagateFields;
   
   vector<uint64_t> localCells = mpiGrid.get_cells();

   calculateBoundaryFlags(mpiGrid,localCells);
   
   // Calculate bit masks used for if-statements by field propagator. 
   // These are used to test whether or not certain combination of 
   // neighbours exists for a cell. These can be replaced by honest 
   // if-statements, but you will just end up needing very many of them 
   // as each bit mask tests the existence of several neighbours at once.
   // Existence of neighbours would also need to be queried from the 
   // parallel grid, i.e. using if-statements is likely to be much 
   // slower.

   if (fieldsArePropagated == true) {
      // x-derivatives are calculated if x-face neighbours exist:
      CALCULATE_DX = 0;
      CALCULATE_DX = CALCULATE_DX | (1 << calcNbrNumber(0,1,1));
      CALCULATE_DX = CALCULATE_DX | (1 << calcNbrNumber(2,1,1));
      
      // y-derivatives are calculated if y-face neighbours exist:
      CALCULATE_DY = 0;
      CALCULATE_DY = CALCULATE_DY | (1 << calcNbrNumber(1,0,1));
      CALCULATE_DY = CALCULATE_DY | (1 << calcNbrNumber(1,2,1));
      
      // z-derivatives are calculated if z-face neighbours exist:
      CALCULATE_DZ = 0;
      CALCULATE_DZ = CALCULATE_DZ | (1 << calcNbrNumber(1,1,0));
      CALCULATE_DZ = CALCULATE_DZ | (1 << calcNbrNumber(1,1,2));
      
      // Edge Ex is calculated i   f -y,-z,+/-x neighbours exist:
      CALCULATE_EX = 0;
      CALCULATE_EX = CALCULATE_EX | (1 << calcNbrNumber(1,0,1)); // -y nbr
      CALCULATE_EX = CALCULATE_EX | (1 << calcNbrNumber(1,1,0)); // -z nbr
      CALCULATE_EX = CALCULATE_EX | (1 << calcNbrNumber(0,1,1)); // -x nbr
      CALCULATE_EX = CALCULATE_EX | (1 << calcNbrNumber(2,1,1)); // +x nbr
       
      // Edge Ey is calculated if -x,-z,+/-y neighbours exist:
      CALCULATE_EY = 0;
      CALCULATE_EY = CALCULATE_EY | (1 << calcNbrNumber(0,1,1)); // -x nbr
      CALCULATE_EY = CALCULATE_EY | (1 << calcNbrNumber(1,1,0)); // -z nbr
      CALCULATE_EY = CALCULATE_EY | (1 << calcNbrNumber(1,0,1)); // -y nbr
      CALCULATE_EY = CALCULATE_EY | (1 << calcNbrNumber(1,2,1)); // +y nbr
       
      // Edge Ez is calculated      if -x,-y,+/-z neighbours exist:
      CALCULATE_EZ = 0;
      CALCULATE_EZ = CALCULATE_EZ | (1 << calcNbrNumber(0,1,1)); // -x nbr
      CALCULATE_EZ = CALCULATE_EZ | (1 << calcNbrNumber(1,0,1)); // -y nbr
      CALCULATE_EZ = CALCULATE_EZ | (1 << calcNbrNumber(1,1,0)); // -z nbr
      CALCULATE_EZ = CALCULATE_EZ | (1 << calcNbrNumber(1,1,2)); // +z nbr

      // Bx is propagated if -x,+/-y,+/-z neighbours exist:
      PROPAGATE_BX = 0;
      PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(0,1,1)); // -x nbr
      PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(1,0,1)); // -y nbr
      PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(1,2,1)); // +y nbr
      PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(1,1,0)); // -z nbr
      PROPAGATE_BX = PROPAGATE_BX | (1 << calcNbrNumber(1,1,2)); // +z nbr
       
      // By is propagated if -y,+/-x,+/-z neighbours exist:
      PROPAGATE_BY = 0;
      PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(1,0,1)); // -y nbr
      PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(0,1,1)); // -x nbr
      PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(2,1,1)); // +x nbr
      PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(1,1,0)); // -z nbr
      PROPAGATE_BY = PROPAGATE_BY | (1 << calcNbrNumber(1,1,2)); // +z nbr
      
      // Bz is propagated if -z,+/-x,+/-y neighbours exist:
      PROPAGATE_BZ = 0;
      PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(1,1,0)); // -z nbr
      PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(0,1,1)); // -x nbr
      PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(2,1,1)); // +x nbr
      PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(1,0,1)); // -y nbr
      PROPAGATE_BZ = PROPAGATE_BZ | (1 << calcNbrNumber(1,2,1)); // +y nbr

      // Calculate derivatives and upwinded edge-E. Exchange derivatives 
      // and edge-E:s between neighbouring processes and calculate 
      // face-averaged E,B fields. Note that calculateUpwindedElectricField 
      // does not exchange edge-E:
      calculateDerivativesSimple(mpiGrid,localCells);
      calculateUpwindedElectricFieldSimple(mpiGrid,localCells);
      calculateVolumeAveragedFields(mpiGrid);
   }

   return true;
}

bool finalizeFieldPropagator(
	dccrg::Dccrg<SpatialCell>& mpiGrid
) {
   return true;
}

static void propagateMagneticField(
	const CellID& cellID,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	creal& dt
);


void calculateDerivativesSimple(
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	const vector<CellID>& localCells
) {
   int timer;
   namespace fs = fieldsolver;
   profile::start("Calculate derivatives");

   // Exchange BX,BY,BZ,RHO,RHOVX,RHOVY,RHOVZ with neighbours
   SpatialCell::set_mpi_transfer_type(Transfer::CELL_B_RHO_RHOV);
   
   timer=profile::initializeTimer("Start comm of B  and RHOV","MPI");
   profile::start(timer);
   mpiGrid.start_remote_neighbor_data_update();
   profile::stop(timer);
   
   timer=profile::initializeTimer("Compute inner cells");
   profile::start(timer);
   // Calculate derivatives on inner cells
   const vector<uint64_t> local_cells = mpiGrid.get_cells_with_local_neighbors();
   for (vector<uint64_t>::const_iterator cell = local_cells.begin(); cell != local_cells.end(); cell++) {
      calculateDerivatives(*cell,mpiGrid);
   }
   profile::stop(timer);

   timer=profile::initializeTimer("Wait for sends","MPI","Wait");
   profile::start(timer);
   mpiGrid.wait_neighbor_data_update_receives();
   profile::stop(timer);

   // Calculate derivatives on boundary cells
   timer=profile::initializeTimer("Compute boundary cells");
   profile::start(timer);
   const vector<uint64_t> boundary_cells = mpiGrid.get_cells_with_remote_neighbor();
   for (vector<uint64_t>::const_iterator cell = boundary_cells.begin(); cell != boundary_cells.end(); cell++) {
       calculateDerivatives(*cell,mpiGrid);
   }
   profile::stop(timer);

   timer=profile::initializeTimer("Wait for sends","MPI","Wait");
   profile::start(timer);
   mpiGrid.wait_neighbor_data_update_sends();
   profile::stop(timer);
   
   profile::stop("Calculate derivatives");
}

void calculateUpwindedElectricFieldSimple(
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	const vector<CellID>& localCells
) {
   namespace fs = fieldsolver;
   int timer;
   profile::start("Calculate upwinded electric field");
   SpatialCell::set_mpi_transfer_type(Transfer::CELL_DERIVATIVES);
   
   timer=profile::initializeTimer("Start communication of derivatives","MPI");
   profile::start(timer);
   mpiGrid.start_remote_neighbor_data_update();
   profile::stop(timer);
   
   timer=profile::initializeTimer("Compute inner cells");
   profile::start(timer);
   // Calculate upwinded electric field on inner cells
   const vector<uint64_t> local_cells = mpiGrid.get_cells_with_local_neighbors();
   for (vector<uint64_t>::const_iterator cell = local_cells.begin(); cell != local_cells.end(); cell++) {
      cuint boundaryFlag = boundaryFlags[*cell];
      if ((boundaryFlag & CALCULATE_EX) == CALCULATE_EX) calculateEdgeElectricFieldX(*cell,mpiGrid);
      if ((boundaryFlag & CALCULATE_EY) == CALCULATE_EY) calculateEdgeElectricFieldY(*cell,mpiGrid);
      if ((boundaryFlag & CALCULATE_EZ) == CALCULATE_EZ) calculateEdgeElectricFieldZ(*cell,mpiGrid);
   }
   profile::stop(timer);
   timer=profile::initializeTimer("Wait for receives","MPI","Wait");
   profile::start(timer);
   mpiGrid.wait_neighbor_data_update_receives();
   profile::stop(timer);
   timer=profile::initializeTimer("Compute boundary cells");
   profile::start(timer);
   // Calculate upwinded electric field on boundary cells:
   const vector<uint64_t> boundary_cells = mpiGrid.get_cells_with_remote_neighbor();
   for (vector<uint64_t>::const_iterator cell = boundary_cells.begin(); cell != boundary_cells.end(); cell++) {
      cuint boundaryFlag = boundaryFlags[*cell];
      if ((boundaryFlag & CALCULATE_EX) == CALCULATE_EX) calculateEdgeElectricFieldX(*cell,mpiGrid);
      if ((boundaryFlag & CALCULATE_EY) == CALCULATE_EY) calculateEdgeElectricFieldY(*cell,mpiGrid);
      if ((boundaryFlag & CALCULATE_EZ) == CALCULATE_EZ) calculateEdgeElectricFieldZ(*cell,mpiGrid);
   }
   profile::stop(timer);
   timer=profile::initializeTimer("Wait for sends","MPI","Wait");
   profile::start(timer);
   mpiGrid.wait_neighbor_data_update_sends();
   profile::stop(timer);
   
   // Exchange electric field with neighbouring processes
   SpatialCell::set_mpi_transfer_type(Transfer::CELL_E);
   timer=profile::initializeTimer("Communicate electric fields","MPI","Wait");
   profile::start(timer);
   mpiGrid.update_remote_neighbor_data();
   profile::stop(timer);

   profile::stop("Calculate upwinded electric field");
}

static void propagateMagneticFieldSimple(
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	creal& dt,
	const vector<CellID>& localCells
) {

   profile::start("Propagate magnetic field");
   // Propagate B on all local cells:
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      propagateMagneticField(cellID,mpiGrid,dt);
   }
   
   // Calculate new B on faces outside the simulation domain using boundary conditions.
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      #ifndef NDEBUG
         const map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
         if (it == boundaryFlags.end()) {cerr << "ERROR Could not find boundary flag for cell #" << cellID << endl; exit(1);}
         cuint existingCells = it->second;
      #else
         cuint existingCells = boundaryFlags[cellID];
      #endif
      cuint nonExistingCells = (existingCells ^ numeric_limits<uint>::max());
      if (mpiGrid[cellID] == NULL) {
         std::cerr << __FILE__ << ":" << __LINE__
            << " No data for cell " << cellID
            << std::endl;
         abort();
      }

      if ((existingCells & PROPAGATE_BX) != PROPAGATE_BX)
	mpiGrid[cellID]->parameters[CellParams::BX] = fieldSolverBoundaryCondBx<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
      if ((existingCells & PROPAGATE_BY) != PROPAGATE_BY)
	mpiGrid[cellID]->parameters[CellParams::BY] = fieldSolverBoundaryCondBy<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
      if ((existingCells & PROPAGATE_BZ) != PROPAGATE_BZ) {
         mpiGrid[cellID]->parameters[CellParams::BZ] = fieldSolverBoundaryCondBz<CellID,uint,Real>(cellID,existingCells,nonExistingCells,mpiGrid);
      }
   }

   profile::stop("Propagate magnetic field");
}

bool propagateFields(
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	creal& dt
) {
   typedef Parameters P;
   // If fields are not propagated, exit immediately. It is user's 
   // responsibility to set correct values for volume-averaged E,B-fields:
   if (fieldsArePropagated == false) return true;

   // Reserve memory for derivatives for all cells on this process:
   vector<CellID> localCells = mpiGrid.get_cells();

   propagateMagneticFieldSimple(mpiGrid,dt,localCells);
   calculateDerivativesSimple(mpiGrid,localCells);
   calculateUpwindedElectricFieldSimple(mpiGrid,localCells);
   calculateVolumeAveragedFields(mpiGrid);
   return true;
}

namespace Rec {
   enum Rec {a_0,a_x,a_y,a_z,a_xx,a_xy,a_xz,
	b_0,b_x,b_y,b_z,b_yx,b_yy,b_yz,
	c_0,c_x,c_y,c_z,c_zx,c_zy,c_zz
   };
}

void reconstructionCoefficients(
	const CellID& cellID,
	const CellID& nbr_i2j1k1,
	const CellID& nbr_i1j2k1,
	const CellID& nbr_i1j1k2,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	Real* result
) {
   // Do not calculate values for non-existing cells:
   if (cellID == INVALID_CELLID) {
      for (int i=0; i<Rec::c_zz+1; ++i) result[i] = 0.0;
      return;
   }
   
   namespace fs = fieldsolver;
   namespace cp = CellParams;
   
   Real* const cep_i1j1k1 = mpiGrid[cellID]->parameters;
   
   // Create a dummy array for containing zero values for cellParams on non-existing cells:
   Real dummyCellParams[CellParams::N_SPATIAL_CELL_PARAMS];
   for (uint i=0; i<CellParams::N_SPATIAL_CELL_PARAMS; ++i) dummyCellParams[i] = 0.0;
   
   Real* cep_i2j1k1 = NULL;
   Real* cep_i1j2k1 = NULL;
   Real* cep_i1j1k2 = NULL;
   if (nbr_i2j1k1 == INVALID_CELLID) cep_i2j1k1 = dummyCellParams;
   else cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->parameters;
   if (nbr_i1j2k1 == INVALID_CELLID) cep_i1j2k1 = dummyCellParams;
   else cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->parameters;
   if (nbr_i1j1k2 == INVALID_CELLID) cep_i1j1k2 = dummyCellParams;
   else cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->parameters;

   #ifndef FS_1ST_ORDER
      creal* const der_i1j1k1 = mpiGrid[cellID]->derivatives;

      // Create a dummy array for containing zero values for derivatives on non-existing cells:
      Real dummyDerivatives[N_SPATIAL_CELL_DERIVATIVES];
      for (uint i=0; i<N_SPATIAL_CELL_DERIVATIVES; ++i) dummyDerivatives[i] = 0.0;
   
      // Fetch neighbour cell derivatives, or in case the neighbour does not 
      // exist, use dummyDerivatives array:
      Real* der_i2j1k1 = NULL;
      Real* der_i1j2k1 = NULL;
      Real* der_i1j1k2 = NULL;
      if (nbr_i2j1k1 == INVALID_CELLID) der_i2j1k1 = dummyDerivatives;
      else der_i2j1k1 = mpiGrid[nbr_i2j1k1]->derivatives;
      if (nbr_i1j2k1 == INVALID_CELLID) der_i1j2k1 = dummyDerivatives;
      else der_i1j2k1 = mpiGrid[nbr_i1j2k1]->derivatives;
      if (nbr_i1j1k2 == INVALID_CELLID) der_i1j1k2 = dummyDerivatives;
      else der_i1j1k2 = mpiGrid[nbr_i1j1k2]->derivatives;

      // Calculate 2nd order reconstruction coefficients:
      result[Rec::a_xy] = der_i2j1k1[fs::dBxdy] - der_i1j1k1[fs::dBxdy];
      CHECK_FLOAT(result[Rec::a_xy])
      result[Rec::a_xz] = der_i2j1k1[fs::dBxdz] - der_i1j1k1[fs::dBxdz];
      CHECK_FLOAT(result[Rec::a_xz])
      result[Rec::a_x ] = (cep_i2j1k1[cp::BX] + cep_i2j1k1[cp::BXFACEX0]) - (cep_i1j1k1[cp::BX] + cep_i1j1k1[cp::BXFACEX0]);
      CHECK_FLOAT(result[Rec::a_x ])
      result[Rec::a_y ] = HALF*(der_i2j1k1[fs::dBxdy] + der_i1j1k1[fs::dBxdy]);
      CHECK_FLOAT(result[Rec::a_y ])
      result[Rec::a_z ] = HALF*(der_i2j1k1[fs::dBxdz] + der_i1j1k1[fs::dBxdz]);
      CHECK_FLOAT(result[Rec::a_z ])
   
      result[Rec::b_yx] = der_i1j2k1[fs::dBydx] - der_i1j1k1[fs::dBydx];
      CHECK_FLOAT(result[Rec::b_yx])
      result[Rec::b_yz] = der_i1j2k1[fs::dBydz] - der_i1j1k1[fs::dBydz];
      CHECK_FLOAT(result[Rec::b_yz])
      result[Rec::b_x ] = HALF*(der_i1j2k1[fs::dBydx] + der_i1j1k1[fs::dBydx]);
      CHECK_FLOAT(result[Rec::b_x ])
      result[Rec::b_y ] = (cep_i1j2k1[cp::BY] + cep_i1j2k1[cp::BYFACEY0]) - (cep_i1j1k1[cp::BY] + cep_i1j1k1[cp::BYFACEY0]);
      CHECK_FLOAT(result[Rec::b_y ])
      result[Rec::b_z ] = HALF*(der_i1j2k1[fs::dBydz] + der_i1j1k1[fs::dBydz]);
      CHECK_FLOAT(result[Rec::b_z ])
   
      result[Rec::c_zx] = der_i1j1k2[fs::dBzdx] - der_i1j1k1[fs::dBzdx];
      CHECK_FLOAT(result[Rec::c_zx])
      result[Rec::c_zy] = der_i1j1k2[fs::dBzdy] - der_i1j1k1[fs::dBzdy];
      CHECK_FLOAT(result[Rec::c_zy])
      result[Rec::c_x ] = HALF*(der_i1j1k2[fs::dBzdx] + der_i1j1k1[fs::dBzdx]);
      CHECK_FLOAT(result[Rec::c_x ])
      result[Rec::c_y ] = HALF*(der_i1j1k2[fs::dBzdy] + der_i1j1k1[fs::dBzdy]);
      CHECK_FLOAT(result[Rec::c_y ])
      result[Rec::c_z ] = (cep_i1j1k2[cp::BZ] + cep_i1j1k2[cp::BZFACEZ0]) - (cep_i1j1k1[cp::BZ] + cep_i1j1k1[cp::BZFACEZ0]);
      CHECK_FLOAT(result[Rec::c_z ])
   
      result[Rec::a_xx] = -HALF*(result[Rec::b_yx] + result[Rec::c_zx]);
      CHECK_FLOAT(result[Rec::a_xx])
      result[Rec::b_yy] = -HALF*(result[Rec::a_xy] + result[Rec::c_zy]);
      CHECK_FLOAT(result[Rec::b_yy])
      result[Rec::c_zz] = -HALF*(result[Rec::a_xz] + result[Rec::b_yz]);
      CHECK_FLOAT(result[Rec::c_zz])
   #else
      for (int i=0; i<Rec::c_zz+1; ++i) result[i] = 0.0;
   #endif
   
   // Calculate 1st order reconstruction coefficients:
   result[Rec::a_0 ] = HALF*(cep_i2j1k1[cp::BX] + cep_i2j1k1[cp::BXFACEX0] + cep_i1j1k1[cp::BX] + cep_i1j1k1[cp::BXFACEX0]) - SIXTH*result[Rec::a_xx];
   CHECK_FLOAT(result[Rec::a_0 ])
   result[Rec::b_0 ] = HALF*(cep_i1j2k1[cp::BY] + cep_i1j2k1[cp::BYFACEY0] + cep_i1j1k1[cp::BY] + cep_i1j1k1[cp::BYFACEY0]) - SIXTH*result[Rec::b_yy];
   CHECK_FLOAT(result[Rec::b_0 ])
   result[Rec::c_0 ] = HALF*(cep_i1j1k2[cp::BZ] + cep_i1j1k2[cp::BZFACEZ0] + cep_i1j1k1[cp::BZ] + cep_i1j1k1[cp::BZFACEZ0]) - SIXTH*result[Rec::c_zz];
   CHECK_FLOAT(result[Rec::c_0 ])
}

void averageFaceXElectricField(
	const CellID& cellID,
	const CellID& nbr_i1j2k1,
	const CellID& nbr_i1j1k2,
	const CellID& nbr_i1j2k2,
	const CellID& nbr_i0j1k1,
	const CellID& nbr_i0j2k1,
	const CellID& nbr_i0j1k2,
	const CellID& nbr_i0j2k2,
	cuint& existingCells,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	Real* result
) {
   namespace fs = fieldsolver;
   namespace cp = CellParams;
   
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   if (mpiGrid[cellID] == NULL) {
      std::cerr << __FILE__ << ":" << __LINE__
         << " No data for cell " << cellID << std::endl;
      abort();
   }
   
   cuint REQUIRED_CELLS = (1 << calcNbrNumber(1,1,1)) | (1 << calcNbrNumber(1,2,1)) | (1 << calcNbrNumber(1,1,2)) | (1 << calcNbrNumber(1,2,2))
     | (1 << calcNbrNumber(0,1,1)) | (1 << calcNbrNumber(0,2,1)) | (1 << calcNbrNumber(0,1,2)) | (1 << calcNbrNumber(0,2,2));

   // If all required neighbour data exists, calculate E vector on 
   // x-face. NEEDS IMPROVEMENT!
   if ((existingCells & REQUIRED_CELLS) == REQUIRED_CELLS) {
      creal* const cep_i1j1k1 = mpiGrid[cellID]->parameters;
      creal* const cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->parameters;
      creal* const cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->parameters;
      creal* const cep_i1j2k2 = mpiGrid[nbr_i1j2k2]->parameters;
      creal* const cep_i0j1k1 = mpiGrid[nbr_i0j1k1]->parameters;
      creal* const cep_i0j2k1 = mpiGrid[nbr_i0j2k1]->parameters;
      creal* const cep_i0j1k2 = mpiGrid[nbr_i0j1k2]->parameters;
      creal* const cep_i0j2k2 = mpiGrid[nbr_i0j2k2]->parameters;
   
      CHECK_FLOAT(cep_i1j1k1[cp::EX])
      CHECK_FLOAT(cep_i1j2k1[cp::EX])
      CHECK_FLOAT(cep_i1j1k2[cp::EX])
      CHECK_FLOAT(cep_i1j2k2[cp::EX])
      CHECK_FLOAT(cep_i0j1k1[cp::EX])
      CHECK_FLOAT(cep_i0j2k1[cp::EX])
      CHECK_FLOAT(cep_i0j1k2[cp::EX])
      CHECK_FLOAT(cep_i0j2k2[cp::EX])
      result[0] = EIGTH*(cep_i1j1k1[cp::EX] + cep_i1j2k1[cp::EX] + cep_i1j1k2[cp::EX] + cep_i1j2k2[cp::EX]
			 + cep_i0j1k1[cp::EX] + cep_i0j2k1[cp::EX] + cep_i0j1k2[cp::EX] + cep_i0j2k2[cp::EX]);

      CHECK_FLOAT(cep_i1j1k1[cp::EY])
      CHECK_FLOAT(cep_i1j1k2[cp::EY])
      result[1] = HALF*(cep_i1j1k1[cp::EY] + cep_i1j1k2[cp::EY]);

      CHECK_FLOAT(cep_i1j1k1[cp::EZ])
      CHECK_FLOAT(cep_i1j2k1[cp::EZ])
      result[2] = HALF*(cep_i1j1k1[cp::EZ] + cep_i1j2k1[cp::EZ]);
      CHECK_FLOAT(result[2])
   } else {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
   }
}

void averageFaceYElectricField(
	const CellID& cellID,
	const CellID& nbr_i2j1k1,
	const CellID& nbr_i1j1k2,
	const CellID& nbr_i2j1k2,
	const CellID& nbr_i1j0k1,
	const CellID& nbr_i2j0k1,
	const CellID& nbr_i1j0k2,
	const CellID& nbr_i2j0k2,
	cuint& existingCells,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	Real* result
) {
   namespace fs = fieldsolver;
   namespace cp = CellParams;
   
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   cuint REQUIRED_CELLS = (1 << calcNbrNumber(1,1,1)) | (1 << calcNbrNumber(2,1,1)) | (1 << calcNbrNumber(1,1,2)) | (1 << calcNbrNumber(2,1,2))
                        | (1 << calcNbrNumber(1,0,1)) | (1 << calcNbrNumber(2,0,1)) | (1 << calcNbrNumber(1,0,2)) | (1 << calcNbrNumber(2,0,2));
   
   // If all required neighbour data exists, calculate E vector on
   // y-face. NEEDS IMPROVEMENT!
   if ((existingCells & REQUIRED_CELLS) == REQUIRED_CELLS) {
      creal* const cep_i1j1k1 = mpiGrid[cellID]->parameters;
      creal* const cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->parameters;
      creal* const cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->parameters;
      creal* const cep_i2j1k2 = mpiGrid[nbr_i2j1k2]->parameters;
      creal* const cep_i1j0k1 = mpiGrid[nbr_i1j0k1]->parameters;
      creal* const cep_i2j0k1 = mpiGrid[nbr_i2j0k1]->parameters;
      creal* const cep_i1j0k2 = mpiGrid[nbr_i1j0k2]->parameters;
      creal* const cep_i2j0k2 = mpiGrid[nbr_i2j0k2]->parameters;
      
      CHECK_FLOAT(cep_i1j1k1[cp::EX])
      CHECK_FLOAT(cep_i1j1k2[cp::EX])
      result[0] = HALF*(cep_i1j1k1[cp::EX] + cep_i1j1k2[cp::EX]);

      CHECK_FLOAT(cep_i1j1k1[cp::EY])
      CHECK_FLOAT(cep_i2j1k1[cp::EY])
      CHECK_FLOAT(cep_i1j1k2[cp::EY])
      CHECK_FLOAT(cep_i2j1k2[cp::EY])
      CHECK_FLOAT(cep_i1j0k1[cp::EY])
      CHECK_FLOAT(cep_i2j0k1[cp::EY])
      CHECK_FLOAT(cep_i1j0k2[cp::EY])
      CHECK_FLOAT(cep_i2j0k2[cp::EY])
      result[1] = EIGTH*(cep_i1j1k1[cp::EY] + cep_i2j1k1[cp::EY] + cep_i1j1k2[cp::EY] + cep_i2j1k2[cp::EY]
		       + cep_i1j0k1[cp::EY] + cep_i2j0k1[cp::EY] + cep_i1j0k2[cp::EY] + cep_i2j0k2[cp::EY]);

      CHECK_FLOAT(cep_i1j1k1[cp::EZ])
      CHECK_FLOAT(cep_i2j1k1[cp::EZ])
      result[2] = HALF*(cep_i1j1k1[cp::EZ] + cep_i2j1k1[cp::EZ]);
   } else {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
   }
}

void averageFaceZElectricField(
	const CellID& cellID,
	const CellID& nbr_i2j1k1,
	const CellID& nbr_i1j2k1,
	const CellID& nbr_i2j2k1,
	const CellID& nbr_i1j1k0,
	const CellID& nbr_i2j1k0,
	const CellID& nbr_i1j2k0,
	const CellID& nbr_i2j2k0,
	cuint& existingCells,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	Real* result
) {
   namespace fs = fieldsolver;
   namespace cp = CellParams;
   
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   if (mpiGrid[cellID] == NULL) {
      std::cerr << __FILE__ << ":" << __LINE__
         << " No data for cell " << cellID << std::endl;
      abort();
   }

   cuint REQUIRED_CELLS = (1 << calcNbrNumber(1,1,1))
      | (1 << calcNbrNumber(2,1,1)) 
      | (1 << calcNbrNumber(1,2,1)) 
      | (1 << calcNbrNumber(2,2,1))
      | (1 << calcNbrNumber(1,1,0))
      | (1 << calcNbrNumber(2,1,0))
      | (1 << calcNbrNumber(1,2,0))
      | (1 << calcNbrNumber(2,2,0));
   
   // If all required neighbour data exists, calculate E vector on
   // z-face. NEEDS IMPROVEMENT!
   if ((existingCells & REQUIRED_CELLS) == REQUIRED_CELLS) {
      creal* const cep_i1j1k1 = mpiGrid[cellID]->parameters;
      creal* const cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->parameters;
      creal* const cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->parameters;
      creal* const cep_i2j2k1 = mpiGrid[nbr_i2j2k1]->parameters;
      creal* const cep_i1j1k0 = mpiGrid[nbr_i1j1k0]->parameters;
      creal* const cep_i2j1k0 = mpiGrid[nbr_i2j1k0]->parameters;
      creal* const cep_i1j2k0 = mpiGrid[nbr_i1j2k0]->parameters;
      creal* const cep_i2j2k0 = mpiGrid[nbr_i2j2k0]->parameters;
      
      result[0] = HALF*(cep_i1j1k1[cp::EX] + cep_i1j2k1[cp::EX]);
      result[1] = HALF*(cep_i1j1k1[cp::EY] + cep_i2j1k1[cp::EY]);

      CHECK_FLOAT(cep_i1j1k1[cp::EZ])
      CHECK_FLOAT(cep_i2j1k1[cp::EZ])
      CHECK_FLOAT(cep_i1j2k1[cp::EZ])
      CHECK_FLOAT(cep_i2j2k1[cp::EZ])
      CHECK_FLOAT(cep_i1j1k0[cp::EZ])
      CHECK_FLOAT(cep_i2j1k0[cp::EZ])
      CHECK_FLOAT(cep_i1j2k0[cp::EZ])
      CHECK_FLOAT(cep_i2j2k0[cp::EZ])
      result[2] = EIGTH*(cep_i1j1k1[cp::EZ] + cep_i2j1k1[cp::EZ] + cep_i1j2k1[cp::EZ] + cep_i2j2k1[cp::EZ]
		       + cep_i1j1k0[cp::EZ] + cep_i2j1k0[cp::EZ] + cep_i1j2k0[cp::EZ] + cep_i2j2k0[cp::EZ]);
      CHECK_FLOAT(result[2])
   } else {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
   }
}

/*!
Averages total magnetic field x component into result.
*/
void averageFaceXMagnField(
	const CellID& cellID,
	const CellID& /*nbr_i2j1k1*/,
	const CellID& /*nbr_i1j2k1*/,
	const CellID& /*nbr_i1j1k2*/,
	creal* const coefficients,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	const int& I,
	Real* result
) {
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   Real*  const cep_i1j1k1 = mpiGrid[cellID]->parameters;

   // Store calculated face-averaged B on x-faces:
   CHECK_FLOAT(cep_i1j1k1[CellParams::BX])
   result[0] = cep_i1j1k1[CellParams::BX] + cep_i1j1k1[CellParams::BXFACEX0];

   CHECK_FLOAT(coefficients[Rec::b_0])
   CHECK_FLOAT(coefficients[Rec::b_x])
   result[1] = coefficients[Rec::b_0] + I*HALF*coefficients[Rec::b_x];

   CHECK_FLOAT(coefficients[Rec::c_0])
   CHECK_FLOAT(coefficients[Rec::c_x])
   result[2] = coefficients[Rec::c_0] + I*HALF*coefficients[Rec::c_x];
}

/*!
Averages total magnetic field y component into result.
*/
void averageFaceYMagnField(
	const CellID& cellID,
	const CellID& /*nbr_i2j1k1*/,
	const CellID& /*nbr_i1j2k1*/,
	const CellID& /*nbr_i1j1k2*/,
	creal* const coefficients,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	const int& J,
	Real* result
) {
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   Real*  const cep_i1j1k1 = mpiGrid[cellID]->parameters;

   // Store calculated face-averaged B on y-faces:
   result[0] = coefficients[Rec::a_0] + J*HALF*coefficients[Rec::a_y];
   result[1] = cep_i1j1k1[CellParams::BY] + cep_i1j1k1[CellParams::BYFACEY0];
   result[2] = coefficients[Rec::c_0] + J*HALF*coefficients[Rec::c_y];
}

/*!
Averages total magnetic field z component into result.
*/
void averageFaceZMagnField(
	const CellID& cellID,
	const CellID& /*nbr_i2j1k1*/,
	const CellID& /*nbr_i1j2k1*/,
	const CellID& /*nbr_i1j1k2*/,
	creal* const coefficients,
	dccrg::Dccrg<SpatialCell>& mpiGrid,
	const int& K,
	Real* result
) {
   if (cellID == INVALID_CELLID) {
      result[0] = 0.0;
      result[1] = 0.0;
      result[2] = 0.0;
      return;
   }

   Real*  const cep_i1j1k1 = mpiGrid[cellID]->parameters;

   // Store calculated face-averaged B on z-faces:
   result[0] = coefficients[Rec::a_0] + K*HALF*coefficients[Rec::a_z];
   result[1] = coefficients[Rec::b_0] + K*HALF*coefficients[Rec::b_z];
   result[2] = cep_i1j1k1[CellParams::BZ] + cep_i1j1k1[CellParams::BZFACEZ0];
}
   
void calculateFaceAveragedFields(
	dccrg::Dccrg<SpatialCell>& mpiGrid
) {
   namespace fs = fieldsolver;
   
   vector<uint64_t> localCells = mpiGrid.get_cells();

   Real faceMagnField[9];
   Real coefficients[Rec::c_zz+1];
   Real coefficients2[Rec::c_zz+1];

   uint existingCells = 0;
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];

      if (mpiGrid[cellID] == NULL) {
         std::cerr << __FILE__ << ":" << __LINE__
            << " No data for cell " << cellID << std::endl;
         abort();
      }
      
      // Get neighbour flags for the cell:
      map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
      if (it == boundaryFlags.end()) existingCells = 0;
      else existingCells = it->second;

      // Get pointer to cellParams array for cell cellID:
      Real* const cellParams = mpiGrid[cellID]->parameters;

      // Calculate reconstruction coefficients for this cell:
      const CellID nbr_i2j1k1 = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2  );
      const CellID nbr_i1j2k1 = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2  );
      const CellID nbr_i1j1k2 = getNeighbourID(mpiGrid, cellID, 2  , 2  , 2+1);
      const CellID nbr_i1j2k2 = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2+1);
      reconstructionCoefficients(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,mpiGrid,coefficients);
      
      // Calculate reconstruction coefficients for i-1 neighbour:
      const CellID nbr_i0j1k1 = getNeighbourID(mpiGrid, cellID, 2-1, 2  , 2  );
      const CellID nbr_i0j2k1 = getNeighbourID(mpiGrid, cellID, 2-1, 2+1, 2  );
      const CellID nbr_i0j1k2 = getNeighbourID(mpiGrid, cellID, 2-1, 2  , 2+1);
      const CellID nbr_i0j2k2 = getNeighbourID(mpiGrid, cellID, 2-1, 2+1, 2+1);
      reconstructionCoefficients(nbr_i0j1k1,cellID,nbr_i0j2k1,nbr_i0j1k2,mpiGrid,coefficients2);
      
      // Calculate B vector on both sides of x-face:
      averageFaceXMagnField(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,coefficients ,mpiGrid,-1,cellParams+CellParams::BXFACEX);
      averageFaceXMagnField(nbr_i0j1k1,cellID,nbr_i0j2k1,nbr_i0j1k2,coefficients2,mpiGrid,+1,faceMagnField+0);
      
      // substract background part from above
      cellParams[CellParams::BXFACEX] -= cellParams[CellParams::BXFACEX0];
      cellParams[CellParams::BYFACEX] -= cellParams[CellParams::BYFACEX0];
      cellParams[CellParams::BZFACEX] -= cellParams[CellParams::BZFACEX0];
      faceMagnField[0] -= cellParams[CellParams::BXFACEX0];
      faceMagnField[1] -= cellParams[CellParams::BYFACEX0];
      faceMagnField[2] -= cellParams[CellParams::BZFACEX0];
      
      // Calculate E vector on x-face (NEEDS IMPROVEMENT):
      averageFaceXElectricField(
        cellID,
        nbr_i1j2k1,
        nbr_i1j1k2,
        nbr_i1j2k2,
        nbr_i0j1k1,
        nbr_i0j2k1,
        nbr_i0j1k2,
        nbr_i0j2k2,
        existingCells,
        mpiGrid,
        cellParams+CellParams::EXFACEX
      );
      
      // Calculate B vector on both sides of y-face:
      const CellID nbr_i2j1k2 = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2+1);
      const CellID nbr_i2j0k1 = getNeighbourID(mpiGrid, cellID, 2+1, 2-1, 2  );
      const CellID nbr_i1j0k1 = getNeighbourID(mpiGrid, cellID, 2  , 2-1, 2  );
      const CellID nbr_i1j0k2 = getNeighbourID(mpiGrid, cellID, 2  , 2-1, 2+1);
      const CellID nbr_i2j0k2 = getNeighbourID(mpiGrid, cellID, 2+1, 2-1, 2+1);
      reconstructionCoefficients(nbr_i1j0k1,nbr_i2j0k1,cellID,nbr_i1j0k2,mpiGrid,coefficients2);
      
      averageFaceYMagnField(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,coefficients ,mpiGrid,-1,cellParams+CellParams::BXFACEY);
      averageFaceYMagnField(nbr_i1j0k1,nbr_i2j0k1,cellID,nbr_i1j0k2,coefficients2,mpiGrid,+1,faceMagnField+3);

      // substract background part from above
      cellParams[CellParams::BXFACEY] -= cellParams[CellParams::BXFACEY0];
      cellParams[CellParams::BYFACEY] -= cellParams[CellParams::BYFACEY0];
      cellParams[CellParams::BZFACEY] -= cellParams[CellParams::BZFACEY0];
      faceMagnField[3] -= cellParams[CellParams::BXFACEY0];
      faceMagnField[4] -= cellParams[CellParams::BYFACEY0];
      faceMagnField[5] -= cellParams[CellParams::BZFACEY0];

      // Calculate E vector on y-face (NEEDS IMPROVEMENT):
      averageFaceYElectricField(
        cellID,
        nbr_i2j1k1,
        nbr_i1j1k2,
        nbr_i2j1k2,
        nbr_i1j0k1,
        nbr_i2j0k1,
        nbr_i1j0k2,
        nbr_i2j0k2,
        existingCells,
        mpiGrid,
        cellParams+CellParams::EXFACEY
      );
      
      // Calculate B vector on both sides of z-face:
      const CellID nbr_i2j2k1 = getNeighbourID(mpiGrid, cellID, 2+1, 2+1, 2  );
      const CellID nbr_i2j1k0 = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2-1);
      const CellID nbr_i1j2k0 = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2-1);
      const CellID nbr_i1j1k0 = getNeighbourID(mpiGrid, cellID, 2  , 2  , 2-1);
      const CellID nbr_i2j2k0 = getNeighbourID(mpiGrid, cellID, 2+1, 2+1, 2-1);
      reconstructionCoefficients(nbr_i1j1k0,nbr_i2j1k0,nbr_i1j2k0,cellID,mpiGrid,coefficients2);
      
      averageFaceZMagnField(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,coefficients ,mpiGrid,-1,cellParams+CellParams::BXFACEZ);
      averageFaceZMagnField(nbr_i1j1k0,nbr_i2j1k0,nbr_i1j2k0,cellID,coefficients2,mpiGrid,+1,faceMagnField+6);

      // substract background part from above
      cellParams[CellParams::BXFACEZ] -= cellParams[CellParams::BXFACEZ0];
      cellParams[CellParams::BYFACEZ] -= cellParams[CellParams::BYFACEZ0];
      cellParams[CellParams::BZFACEZ] -= cellParams[CellParams::BZFACEZ0];
      faceMagnField[6] -= cellParams[CellParams::BXFACEZ0];
      faceMagnField[7] -= cellParams[CellParams::BYFACEZ0];
      faceMagnField[8] -= cellParams[CellParams::BZFACEZ0];

      // Calculate E vector on z-face (NEEDS IMPROVEMENT):
      averageFaceZElectricField(
        cellID,
        nbr_i2j1k1,
        nbr_i1j2k1,
        nbr_i2j2k1,
        nbr_i1j1k0,
        nbr_i2j1k0,
        nbr_i1j2k0,
        nbr_i2j2k0,
        existingCells,
        mpiGrid,
        cellParams+CellParams::EXFACEZ
      );
      
      // Store the average value (maybe should store upwinded value?): 
      for (uint i=0; i<9; ++i) {
	cellParams[CellParams::BXFACEX+i] = HALF*(cellParams[CellParams::BXFACEX+i] + faceMagnField[i]);
      }
   }
}

void calculateVolumeAveragedFields(
	dccrg::Dccrg<SpatialCell>& mpiGrid
) {
   // If fields are not propagated exit immediately. It is 
   // user's responsibility to set correct values to volume-averaged E,B-fields:
   
   if (fieldsArePropagated == false) return;
   profile::start("Calculate volume averaged fields");
   
   namespace fs = fieldsolver;
   namespace cp = CellParams;
                                   
   vector<uint64_t> localCells = mpiGrid.get_cells();
   
   Real coefficients[Rec::c_zz+1];
      
   cuint EX_CELLS = (1 << calcNbrNumber(1,1,1))
   	| (1 << calcNbrNumber(1,2,1))
   	| (1 << calcNbrNumber(1,1,2))
   	| (1 << calcNbrNumber(1,2,2));
   cuint EY_CELLS = (1 << calcNbrNumber(1,1,1))
   	| (1 << calcNbrNumber(2,1,1))
   	| (1 << calcNbrNumber(1,1,2))
   	| (1 << calcNbrNumber(2,1,2));
   cuint EZ_CELLS = (1 << calcNbrNumber(1,1,1))
   	| (1 << calcNbrNumber(2,1,1))
   	| (1 << calcNbrNumber(1,2,1))
   	| (1 << calcNbrNumber(2,2,1));
   
   uint existingCells = 0;
   for (size_t cell=0; cell<localCells.size(); ++cell) {
      const CellID cellID = localCells[cell];
      
      // Get neighbour flags for the cell:
      map<CellID,uint>::const_iterator it = boundaryFlags.find(cellID);
      if (it == boundaryFlags.end()) existingCells = 0;
      else existingCells = it->second;
      
      // Calculate reconstruction coefficients for this cell:
      const CellID nbr_i2j1k1 = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2  );
      const CellID nbr_i1j2k1 = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2  );
      const CellID nbr_i1j1k2 = getNeighbourID(mpiGrid, cellID, 2  , 2  , 2+1);
      reconstructionCoefficients(cellID,nbr_i2j1k1,nbr_i1j2k1,nbr_i1j1k2,mpiGrid,coefficients);
      
      // Calculate volume average of B:
      Real* const cellParams = mpiGrid[cellID]->parameters;
      cellParams[cp::BXVOL] = coefficients[Rec::a_0] - cellParams[cp::BXVOL0];
      cellParams[cp::BYVOL] = coefficients[Rec::b_0] - cellParams[cp::BYVOL0];
      cellParams[cp::BZVOL] = coefficients[Rec::c_0] - cellParams[cp::BZVOL0];
      
      // Calculate volume average of E (NEEDS IMPROVEMENT):
      const CellID nbr_i1j2k2 = getNeighbourID(mpiGrid, cellID, 2  , 2+1, 2+1);
      const CellID nbr_i2j1k2 = getNeighbourID(mpiGrid, cellID, 2+1, 2  , 2+1);
      const CellID nbr_i2j2k1 = getNeighbourID(mpiGrid, cellID, 2+1, 2+1, 2  );
      creal* const cep_i1j1k1 = cellParams;
      
      if ((existingCells & EX_CELLS) == EX_CELLS) {
	 creal* const cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->parameters;
	 creal* const cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->parameters;
	 creal* const cep_i1j2k2 = mpiGrid[nbr_i1j2k2]->parameters;

         CHECK_FLOAT(cep_i1j1k1[cp::EX])
         CHECK_FLOAT(cep_i1j2k1[cp::EX])
         CHECK_FLOAT(cep_i1j1k2[cp::EX])
         CHECK_FLOAT(cep_i1j2k2[cp::EX])
	 cellParams[cp::EXVOL] = FOURTH*(cep_i1j1k1[cp::EX] + cep_i1j2k1[cp::EX] + cep_i1j1k2[cp::EX] + cep_i1j2k2[cp::EX]);
	 CHECK_FLOAT(cellParams[cp::EXVOL])
      } else {
	 cellParams[cp::EXVOL] = 0.0;
      }
      
      if ((existingCells & EY_CELLS) == EY_CELLS) {
	 creal* const cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->parameters;
	 creal* const cep_i1j1k2 = mpiGrid[nbr_i1j1k2]->parameters;
	 creal* const cep_i2j1k2 = mpiGrid[nbr_i2j1k2]->parameters;

         CHECK_FLOAT(cep_i1j1k1[cp::EY])
         CHECK_FLOAT(cep_i2j1k1[cp::EY])
         CHECK_FLOAT(cep_i1j1k2[cp::EY])
         CHECK_FLOAT(cep_i2j1k2[cp::EY])
	 cellParams[cp::EYVOL] = FOURTH*(cep_i1j1k1[cp::EY] + cep_i2j1k1[cp::EY] + cep_i1j1k2[cp::EY] + cep_i2j1k2[cp::EY]);
	 CHECK_FLOAT(cellParams[cp::EYVOL])
      } else {
	 cellParams[cp::EYVOL] = 0.0;
      }
      
      if ((existingCells & EZ_CELLS) == EZ_CELLS) {
	 creal* const cep_i2j1k1 = mpiGrid[nbr_i2j1k1]->parameters;
	 creal* const cep_i1j2k1 = mpiGrid[nbr_i1j2k1]->parameters;
	 creal* const cep_i2j2k1 = mpiGrid[nbr_i2j2k1]->parameters;

	 CHECK_FLOAT(cep_i1j1k1[cp::EZ])
	 CHECK_FLOAT(cep_i2j1k1[cp::EZ])
	 CHECK_FLOAT(cep_i1j2k1[cp::EZ])
	 CHECK_FLOAT(cep_i2j2k1[cp::EZ])
	 cellParams[cp::EZVOL] = FOURTH*(cep_i1j1k1[cp::EZ] + cep_i2j1k1[cp::EZ] + cep_i1j2k1[cp::EZ] + cep_i2j2k1[cp::EZ]);
	 CHECK_FLOAT(cellParams[cp::EZVOL])
      } else {
	 cellParams[cp::EZVOL] = 0.0;
      }
   }
   profile::stop("Calculate volume averaged fields");
}

