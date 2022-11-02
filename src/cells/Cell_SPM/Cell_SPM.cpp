/*
 * Cell_SPM.cpp
 *
 * Implements the functions for the parent class of the Cells
 *
 * Copyright (c) 2019, The Chancellor, Masters and Scholars of the University
 * of Oxford, VITO nv, and the 'Slide' Developers.
 * See the licence file LICENCE.txt for more information.
 */

#include "Cell_SPM.hpp"

#include <cassert>
#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <array>
#include <algorithm>
#include <utility>

namespace slide {

double Cell_SPM::getRdc() noexcept
{
  const double surf_sei = st.an() * geo.elec_surf * st.thickn(); //!< real surface area of the sei-layer
  const double surf_p = st.ap() * geo.elec_surf * st.thickp();   //!< real surface area of the positive electrode
  const double surf_n = st.an() * geo.elec_surf * st.thickn();   //!< real surface area of the negative electrode

  //!< calculate the resistance from every component
  const double Rdc_sei = st.delta() * rsei / surf_sei;
  const double Rdc_p = st.rDCp() / surf_p;
  const double Rdc_n = st.rDCn() / surf_n;
  const double Rdc_cc = st.rDCcc() / geo.elec_surf;

  return (Rdc_sei + Rdc_p + Rdc_n + Rdc_cc);
}

Status Cell_SPM::setSOC(double SOCnew, bool checkV, bool print) //!< Also not used except test functions.
{
  /*
   * checkV	true, the voltage is checked after setting the current
   * 				if it is outside the safety limits of the cell, error 3 is thrown and the old SOC is restored
   * 				if it is outside the valid limits of the cell, error 2 is thrown but the new SOC is kept
   * 				if inside allowed Vrange, it returns the voltage
   * 			false, the voltage is not checked (function returns 0, no errors are thrown)
   *  		if no value of checkV is given, it is set to true
   * print 	controls the printing of error messages
   * 			if true, error messages are printed (if the global printing variable is high enough)
   * 			if false, no messages are printed, but the errors are still thrown
   * 			if no value, the default is true
   *
   * THROWS
   * 10 	illegal SOC, values must be between 0 and 1
   */

  if (!free::check_SOC(SOCnew))
    return Status::SOC_limits_violation;

  const double SOCold = st.SOC();

  st.SOC() = SOCnew;

  if (checkV) {
    double v;
    const auto status = checkVoltage(v, print); //!< get the voltage Does not throw anymore!

    if (isStatusBad(status))
      st.SOC() = SOCold; //!< Restore states here.

    return status;
  }

  return Status::Success;
}

Status Cell_SPM::setCurrent(double Inew, bool checkV, bool print)
{
  /*
   * sets the current
   *
   * checkV	true, the voltage is checked after setting the current
   * 				if it is outside the safety limits of the cell, error 3 is thrown and the old current is restored
   * 				if it is outside the valid limits of the cell, error 2 is thrown but the new current is kept
   * 				if inside allowed Vrange, it returns the voltage
   * 			false, the voltage is not checked (function returns 0, no errors are thrown)
   *  		if no value of checkV is given, it is set to true
   * print 	controls the printing of error messages
   * 			if true, error messages are printed (if the global printing variable is high enough)
   * 			if false, no messages are printed, but the errors are still thrown
   * 			if no value, the default is true
   *
   * returns the voltage if checkV = true, else it returns 0
   *
   * THROWS
   * 2 	checkV is true && the voltage is outside the allowed range but still in the safety range
   * 			and current is in the correct direction. I.e. if charging and V > Vmax or discharging and V < Vmin
   * 3 	checkV is true && the voltage is outside the safety limits, old current is restored
   * 			and current is in the correct direction. I.e. if charging and V > VMAX or discharging and V < VMAX
   * 		if currents are in the 'wrong' direction (e.g. charging but V < Vmin or V < VMIN) then don't throw errors
   * 			since this current is helping to rectify the situation
   */

  const auto old_I = st.I();
  const auto old_V = st.V();
  const auto Vcell_valid_old = Vcell_valid;

  st.I() = Inew;
  Vcell_valid = false;

  const auto status = checkCurrent(checkV, print);

  if (isStatusBad(status)) {

    st.I() = old_I;
    st.V() = old_V;
    Vcell_valid = Vcell_valid_old; //!< #TODO in future make it into states.
  }

  return status;
}

bool Cell_SPM::getCSurf(double &cps, double &cns, bool print)
{
  /*
   * Calculates the surface concentration at each particle.
   * Uses the matrices from the Model struct with the spatial discretisation for the solid diffusion PDE.
   *
   * IN
   * print 	boolean indicating if we want to print error messages or not
   *
   * OUT
   * cps 	surface li-concentration at the positive particle [mol m-3]
   * cns 	surface li-concentration at the negative particle [mol m-3]
   */

  const auto [Dpt, Dnt] = calcDiffConstant();
  const auto [i_app, jp, jn] = calcMolarFlux(); //!< current density, molar flux on the pos/neg particle

  std::tie(cps, cns) = calcSurfaceConcentration(jp, jn, Dpt, Dnt);

  //!< check if the surface concentration is within the allowed range
  //!< 	0 < cp < Cmaxpos  &&  0 < cn < Cmaxneg
  const auto flag = !(cps <= 0 || cns <= 0 || cps >= Cmaxpos || cns >= Cmaxneg);

  return flag;
}

void Cell_SPM::getC(double cp[], double cn[]) noexcept
{
  /*
   * Calculates the lithium concentration at each positive Chebyshev node (0 <= x <= 1), including the centre and surface nodes.
   * Uses the matrices from the state space matrices.
   * See the equations in the explanatory documents.
   *
   * OUT
   * cp 	li-concentration at each Chebyshev node in the positive electrode [mol m-3], length of the array should be nch+2
   * 			cp[0]			concentration at the surface of the sphere
   * 			cp[1 to nch]	concentration at the inner nodes
   * 			cp[nch + 1]		concentration at the centre of the sphere
   * cn 	li-concentration at each Chebyshev node in the negative electrode [mol m-3], length of the array should be nch+2
   * 			cn[0]			concentration at the surface of the sphere
   * 			cn[1 to nch]	concentration at the inner nodes
   * 			cn[nch + 1]		concentration at the centre of the sphere
   *
   */
  using settings::nch;

  //!< Calculate the diffusion constant at the battery temperature using an Arrhenius relation
  const auto [Dpt, Dnt] = calcDiffConstant();

  //!< Calculate the molar flux on the surfaces
  const auto [i_app, jp, jn] = calcMolarFlux(); //!< current density, molar flux on the pos/neg particle

  //!< Calculate concentration at the surface and inner nodes using the matrices from the spatial discretisation of the solid diffusion PDE
  //!< 	cp = M->Cp[:][:] * zp[:] + M->Dp*jp/Dpt
  //!< 	cn = M->Cn[:][:] * zn[:] + M->Dn*jn/Dnt
  for (size_t i = 0; i < nch + 1; i++) //!< Problem here!!!!!!! #TODO
  {                                    //!< loop to calculate at each surface + inner node
    double cpt{ 0 }, cnt{ 0 };
    for (unsigned j = 0; j < nch; j++) {
      cpt += M->Cp[i][j] * st.zp(j);
      cnt += M->Cn[i][j] * st.zn(j);
    }
    cp[i] = cpt + M->Dp[i] * jp / Dpt;
    cn[i] = cnt + M->Dn[i] * jn / Dnt;
  }

  //!< Calculate the concentration at centre node using the boundary condition (the concentration gradient at the centre has to be 0 due to symmetry)
  //!< cp_centre = -1/2 (M->Cc[:]*cp +jp*Rp/Dpt)
  //!< cn_centre = -1/2 (M->Cc[:]*cn +jn*Rn/Dnt)
  double cpt{ 0 }, cnt{ 0 };
  for (size_t i = 0; i < nch + 1; i++) {
    cpt += M->Cc[i] * cp[i];
    cnt += M->Cc[i] * cn[i];
  }
  cp[nch + 1] = (-1.0 / 2.0) * (cpt + jp * geo.Rp / Dpt);
  cn[nch + 1] = (-1.0 / 2.0) * (cnt + jn * geo.Rn / Dnt);
}

double Cell_SPM::getOCV(bool print)
{
  /*
   * print 	controls the printing of error messages, (default = true)
   * 			if the SOC is out of the range, an error is always thrown (1)
   * 				if crit is true an error message is printed (if verbose is above the critical level)
   * 				else no error messages are printed
   * 			this is an optional argument. If no value is given, it is assumed to be true
   * 				this can be overwritten by giving an argument of false
   *
   * THROWS
   * 1 	if SOC is outside the allowed range
   * 			passed on from linear interpolation
   */

#if TIMING
  Clock clk;
#endif

  const bool verb = settings::printBool::printCrit && print; //!< print if the (global) verbose-setting is above the threshold

  //!< Get the surface concentrations
  double cps, cns;
  const auto flag = getCSurf(cps, cns, verb);
  //!< Calculate the li-fraction (instead of the li-concentration)
  const double zp_surf = (cps / Cmaxpos);
  const double zn_surf = (cns / Cmaxneg);
  const bool bound = true;                                               //!< in linear interpolation, throw an error if you are out of the allowed range
  const double dOCV = OCV_curves.dOCV_tot.interp(zp_surf, print, bound); //!< entropic coefficient of the total cell voltage [V/K]
  const double OCV_n = OCV_curves.OCV_neg.interp(zn_surf, print, bound); //!< anode potential [V]
  const double OCV_p = OCV_curves.OCV_pos.interp(zp_surf, print, bound); //!< cathode potential [V]

#if TIMING
  timeData.getOCV += clk.duration();
#endif
  return (OCV_p - OCV_n + (st.T() - T_ref) * dOCV);
}

double Cell_SPM::V(bool print)
{
  /*
   * Function to calculate the cell voltage
   *
   * IN
   * print 	boolean indicating if we want to print error messages or not
   * 				if true, error messages are printed
   * 				if false no error messages are printed (but the error will still be thrown)
   * 			we need this input from higher level functions because at this point we cannot know if an error will be critical or not
   *
   * OUT
   * V 		battery voltage [V]
   * bool 	indicates if the voltage is in the allowed range Vmin <= V <= Vmax
   *
   * THROWS
   */

#if TIMING
  Clock clk;
#endif

  //!< If the stored value is the most up to date one, then simply return this value
  if (Vcell_valid)
    return st.V();

  const bool verb = settings::printBool::printCrit && print; //!< print if the (global) verbose-setting is above the threshold

  //!< Get the surface concentrations
  double cps, cns;
  const auto flag = getCSurf(cps, cns, verb);
  //!< check if the surface concentration is within the allowed range
  //!< 	0 < cp < Cmaxpos
  //!< 	0 < cn < Cmaxneg
  //!< don't allow 0 or Cmax because in that case, i0 will be 0, and the overpotentials will have 1/0 = inf or nan
  if (!flag) {
    if (verb) { //!< print error message unless you want to suppress the output
      std::cerr << "ERROR in Cell_SPM::V: concentration out of bounds. the positive lithium fraction is "
                << cps / Cmaxpos << " and the negative lithium fraction is " << cns / Cmaxneg
                << " they should both be between 0 and 1.\n";
    }
    //	*v = nan("double"); //!< set the voltage to nan (Not A Number)
    return 0; //!< Surface concentration is out of bounds.
  } else {
    //!< Calculate the li-fraction (instead of the li-concentration)
    const double zp_surf = (cps / Cmaxpos);
    const double zn_surf = (cns / Cmaxneg);

    //!< Calculate the electrode potentials
    const bool bound = true;                                              //!< in linear interpolation, throw an error if you are out of the allowed range
    const double dOCV = OCV_curves.dOCV_tot.interp(zp_surf, verb, bound); //!< entropic coefficient of the total cell voltage [V/K]
    const double OCV_n = OCV_curves.OCV_neg.interp(zn_surf, verb, bound); //!< anode potential [V]
    const double OCV_p = OCV_curves.OCV_pos.interp(zp_surf, verb, bound); //!< cathode potential [V]

    const double i_app = I() / geo.elec_surf; //!< current density on the electrodes [I m-2]

    const auto [etapi, etani] = calcOverPotential(cps, cns, i_app);

    //!< Calculate the cell voltage
    //!< the cell OCV at the reference temperature is OCV_p - OCV_n
    //!< this OCV is adapted to the actual cell temperature using the entropic coefficient dOCV * (T - Tref)
    //!< then the overpotentials and the resistive voltage drop are added
    const auto entropic_effect = (st.T() - T_ref) * dOCV;                          //!< (st.T() - T_ref) * dOCV; #TODO entropic effect is zero.
    st.V() = (OCV_p - OCV_n + entropic_effect) + (etapi - etani) - getRdc() * I(); //#TODO make Vcell_valid true.
    Vcell_valid = true;                                                            //!< we now have the most up to date value stored

    //!< //!< make the output variables
    //!< *OCVp = OCV_p;
    //!< *OCVn = OCV_n;
    //!< *etap = etapi;
    //!< *etan = etani;
    //!< *Rdrop = -getR() * Icell;
    //!< *Temp = st.T();

    //!< if constexpr (settings::printBool::printCellFunctions)
    //!< 	std::cout << "Cell_SPM::getVoltage terminating with V = " << *v << " and valid is " << (Vmin <= *v && *v <= Vmax) << '\n';

    //!< Return whether this voltage is within the allowed limits or not
    //!< return (*v > Vmax) - (*v < Vmin);
  }

#if TIMING
  timeData.V += clk.duration(); //!< time in seconds
#endif

  return st.V();
}

Cell_SPM::Cell_SPM() : Cell() //!< Default constructor
{
  //!< ID string
  ID = "Cell_SPM";

  OCV_curves = OCVcurves::makeOCVcurves(cellType::KokamNMC);

  //!< Set initial state:
  st.T() = settings::T_ENV;
  st.delta() = 1e-9;
  st.LLI() = 0; //!< lost lithium. Start with 0 so we can keep track of how much li we lose while cycling the cell

  st.thickp() = 70e-6;                                    //!< thickness of the positive electrode
  st.thickn() = 73.5e-6;                                  //!< thickness of the negative electrode
  st.ep() = 0.5;                                          //!< volume fraction of active material in the cathode
  st.en() = 0.5;                                          //!< volume fraction of active material in the anode
  st.ap() = 3 * st.ep() / geo.Rp;                         //!< effective surface area of the cathode, the 'real' surface area is the product of the effective surface area (a) with the electrode volume (elec_surf * thick)
  st.an() = 3 * st.en() / geo.Rn;                         //!< effective surface area of the anode
  st.CS() = 0.01 * st.an() * geo.elec_surf * st.thickn(); //!< initial crack surface. Start with 1% of the real surface area
  st.Dp() = 8e-14;                                        //!< diffusion constant of the cathode at reference temperature
  st.Dn() = 7e-14;                                        //!< diffusion constant of the anode at reference temperature

  //!< R = Rdc * (thickp * ap * elec_surf + thickn * an * elec_surf) / 2; //!< specific resistance of the combined electrodes, see State::iniStates
  st.rDCp() = 2.8e-3;
  st.rDCn() = 2.8e-3;
  st.rDCcc() = 232.5e-6; //!< note: this is divided by geometric surface (elec_surf) instead of effective surf (a*thick*elec_surf)

  st.delta_pl() = 0; //!< thickness of the plated lithium layer. You can start with 0 here

  constexpr double fp = 0.689332; //!< lithium fraction in the cathode at 50% soc (3.68136 V) [-]
  constexpr double fn = 0.479283; //!< lithium fraction in the anode at 50% soc (3.68136 V) [-]
  setC(fp, fn);
  st.SOC() = 0.5; //!< Since fp and fn are set at 50%.

  s_ini = st; //!< set the states, with a random value for the concentration

  //!< Default values for not defined other param:

  csparam.CS4Amax = 5 * st.an() * st.thickn() * geo.elec_surf; //!< assume the maximum crack surface is 5 times the initial anode surface

  cellData.initialise(*this);
}

//!< int Cell_SPM::getVoltage_ne(bool print, double *v, double *OCVp, double *OCVn, double *etap, double *etan, double *Rdrop, double *Temp)
//!< {
//!< 	/*
//!< 	 * Function to calculate the cell voltage and give detailed feedback about the cell
//!< 	 *
//!< 	 * IN
//!< 	 * print 	boolean indicating if we want to print error messages or not
//!< 	 * 				if true, error messages are printed
//!< 	 * 				if false no error messages are printed (but the error will still be thrown)
//!< 	 * 			we need this input from higher level functions because at this point we cannot know if an error will be critical or not
//!< 	 *
//!< 	 * OUT
//!< 	 * V 		battery voltage [V]
//!< 	 * OCVp		cathode potential [V]
//!< 	 * OCVn		anode potential [V]
//!< 	 * etap 	overpotential at positive electrode [V], < 0 on discharge
//!< 	 * etan 	overpotential at negative electrode [V],  > 0 on discharge
//!< 	 * Rdrop 	resistive voltage drop [V], < 0 on discharge, > 0 on charge
//!< 	 * Temp 	cell temperature [K]
//!< 	 * bool 	indicates if the voltage is in the allowed range Vmin <= V <= Vmax
//!< 	 *
//!< 	 * THROWS
//!< 	 * 101		invalid surface concentration detected
//!< 	 */

//!< 	//!< #HOTFUNC
//!< 	using namespace PhyConst;

//!< 	if constexpr (settings::printBool::printCellFunctions)
//!< 		std::cout << "Cell_SPM::getVoltage starting\n";

//!< 	//!< Get the surface concentrations
//!< 	double cps, cns;
//!< 	const auto flag = getCSurf(cps, cns, print);

//!< 	//!< check if the surface concentration is within the allowed range
//!< 	//!< 	0 < cp < Cmaxpos
//!< 	//!< 	0 < cn < Cmaxneg
//!< 	//!< don't allow 0 or Cmax because in that case, i0 will be 0, and the overpotentials will have 1/0 = inf or nan
//!< 	if (!flag)
//!< 	{
//!< 		if (print) //!< print error message unless you want to suppress the output
//!< 			std::cerr << "ERROR in Cell_SPM::getVoltage: concentration out of bounds. "
//!< 					  << "the positive lithium fraction is " << cps / Cmaxpos
//!< 					  << " and the negative lithium fraction is " << cns / Cmaxneg
//!< 					  << " they should both be between 0 and 1.\n";

//!< 		*v = nan("double"); //!< set the voltage to nan (Not A Number)
//!< 		return 2;			//!< Surface concentration is out of bounds.
//!< 	}
//!< 	else
//!< 	{
//!< 		//!< Calculate the li-fraction (instead of the li-concentration)
//!< 		const double zp_surf = (cps / Cmaxpos);
//!< 		const double zn_surf = (cns / Cmaxneg);

//!< 		//!< Calculate the electrode potentials
//!< 		const bool bound = true;											   //!< in linear interpolation, throw an error if you are out of the allowed range
//!< 		const double dOCV = OCV_curves.dOCV_tot.interp(zp_surf, print, bound); //!< entropic coefficient of the total cell voltage [V/K]
//!< 		const double OCV_n = OCV_curves.OCV_neg.interp(zn_surf, print, bound); //!< anode potential [V]
//!< 		const double OCV_p = OCV_curves.OCV_pos.interp(zp_surf, print, bound); //!< cathode potential [V]

//!< 		const double i_app = Icell / elec_surf; //!< current density on the electrodes [I m-2]

//!< 		const auto [etapi, etani] = calcOverPotential(cps, cns, i_app);

//!< 		//!< Calculate the cell voltage
//!< 		//!< the cell OCV at the reference temperature is OCV_p - OCV_n
//!< 		//!< this OCV is adapted to the actual cell temperature using the entropic coefficient dOCV * (T - Tref)
//!< 		//!< then the overpotentials and the resistive voltage drop are added
//!< 		*v = (OCV_p - OCV_n + (st.T() - T_ref) * dOCV) + (etapi - etani) - getRdc() * Icell;

//!< 		//!< make the output variables
//!< 		*OCVp = OCV_p;
//!< 		*OCVn = OCV_n;
//!< 		*etap = etapi;
//!< 		*etan = etani;
//!< 		*Rdrop = -getRdc() * Icell;
//!< 		*Temp = st.T();

//!< 		if constexpr (settings::printBool::printCellFunctions)
//!< 			std::cout << "Cell_SPM::getVoltage terminating with V = " << *v << " and valid is "
//!< 					  << (Vmin() <= *v && *v <= Vmax()) << '\n';

//!< 		//!< Return whether this voltage is within the allowed limits or not
//!< 		return (*v > Vmax()) - (*v < Vmin());
//!< 	}
//!< }

//!< int Cell_SPM::getVoltage(bool print, double *v, double *OCVp, double *OCVn, double *etap, double *etan, double *Rdrop, double *Temp)
//!< {
//!< 	/*
//!< 	 * Function to calculate the cell voltage and give detailed feedback about the cell
//!< 	 *
//!< 	 * IN
//!< 	 * print 	boolean indicating if we want to print error messages or not
//!< 	 * 				if true, error messages are printed
//!< 	 * 				if false no error messages are printed (but the error will still be thrown)
//!< 	 * 			we need this input from higher level functions because at this point we cannot know if an error will be critical or not
//!< 	 *
//!< 	 * OUT
//!< 	 * V 		battery voltage [V]
//!< 	 * OCVp		cathode potential [V]
//!< 	 * OCVn		anode potential [V]
//!< 	 * etap 	overpotential at positive electrode [V], < 0 on discharge
//!< 	 * etan 	overpotential at negative electrode [V],  > 0 on discharge
//!< 	 * Rdrop 	resistive voltage drop [V], < 0 on discharge, > 0 on charge
//!< 	 * Temp 	cell temperature [K]
//!< 	 * bool 	indicates if the voltage is in the allowed range Vmin <= V <= Vmax
//!< 	 *
//!< 	 * THROWS
//!< 	 * 101		invalid surface concentration detected
//!< 	 */

//!< 	//!< Throwing version of getVoltage_ne;

//!< 	auto status = getVoltage_ne(print, V, OCVp, OCVn, etap, etan, Rdrop, Temp);

//!< 	if (status == 2)
//!< 	{
//!< 		throw 101;
//!< 	}

//!< 	return status;
//!< }

Status Cell_SPM::setStates(setStates_t s, bool checkV, bool print)
{
  /*
   * Returns Status see Status.hpp for the meaning.
   */

#if TIMING
  Clock clk;
#endif
  auto st_old = st; //!< Back-up values.

  std::copy(s.begin(), s.begin() + st.size(), st.begin()); //!< Copy states.
  s = s.last(s.size() - st.size());                        //!< Remove first Nstates elements from span.
  Vcell_valid = false;
  const Status status = free::check_Cell_states(*this, checkV);

  if (isStatusBad(status)) {
    st = st_old;        //!< Restore states here.
    Vcell_valid = true; //!< #TODO if this is ok.
  }
#if TIMING
  timeData.setStates += clk.duration(); //!< time in seconds
#endif
  return status;
}

bool Cell_SPM::validStates(bool print)
{
  /*
   * note: does NOT check the voltage, only whether all fields are in the allowed range
   */

#if TIMING
  Clock clk;
#endif
  const bool verb = print && settings::printBool::printCrit; //!< print if the (global) verbose-setting is above the threshold

  //!< Check if all fields are present & extract their values

  constexpr double tol = 1e-5;

  bool range = free::check_SOC(SOC()); //!< check whether SoC in the allowed range

  if (T() < Tmin() || T() > Tmax()) {
    if (verb)
      std::cerr << "ERROR in Cell_SPM::validState, T is outside of the range, " << Tmin()
                << " <= T <= " << Tmax() << ", value is " << T() << '\n';
    range = false;
  }

  double cps, cns;
  auto flag = getCSurf(cps, cns, false);

  if (flag) {
    if (verb)
      std::cerr << "ERROR in Cell_SPM::validState: concentration out of bounds. the positive lithium fraction is "
                << cps / Cmaxpos << " and the negative lithium fraction is " << cns / Cmaxneg
                << " they should both be between 0 and 1.\n";
    range = false;
  }

  const double app = 3 * st.ep() / geo.Rp;

  if (std::abs(st.ap() - app) / st.ap() > tol) {
    if (verb)
      std::cerr << "SOME ERROR #TODO!\n";
    range = false;
  }

  const double ann = 3 * st.en() / geo.Rn;

  if (std::abs(st.an() - ann) / st.an() > tol) {
    if (verb)
      std::cerr << "SOME ERROR #TODO!\n";
    range = false;
  }
#if TIMING
  timeData.validStates += clk.duration(); //!< time in seconds
#endif
  //!< there is no range on the current
  return range;
}

void Cell_SPM::setTenv(double Tenv)
{
  /*
   * Sets the environmental temperature
   *
   * IN
   * Tenv 	environmental temperature, 273 <= T <= 333 [K]
   *
   * THROWS
   * 102 		illegal value of T
   */

  //!< check if the environmental temperature is in the allowed limits
  bool T = (Tenv < Tmin()) || (Tenv > Tmax()); //!< the temperature limits are defined in State.hpp
  if (T) {
    std::cerr << "ERROR in Cell_SPM::setTenv, illegal value of environmental temperature " << Tenv
              << "K. The value has to be between The value has to be between " << Tmin()
              << "and " << Tmax() << ".\n";
    std::cout << "Throwed in File: " << __FILE__ << ", line: " << __LINE__ << '\n';
    throw 102;
  }

  //!< update the temperature
  T_env = Tenv;
}

//!< void Cell_SPM::setVlimits(double VMAX, double VMIN)
//!< {
//!< 	/*
//!< 	 * sets the voltage limits of this cell to the given values.
//!< 	 *
//!< 	 * IN
//!< 	 * VMAX		maximum voltage of this cell, [V]
//!< 	 * VMIN 	minimum voltage of this cell, [V]
//!< 	 *
//!< 	 * THROWS
//!< 	 * 103 		illegal value of the voltage limits
//!< 	 * 			Both values have to be between the maximum and minimum of the OCV curve of the cell
//!< 	 * 			We are not checking the full OCV curve because that would take too long.
//!< 	 * 			Instead, we check that the values are positive and that VMAX is below the maximum of the cathode OCV.
//!< 	 */

//!< 	if constexpr (settings::printBool::printCellFunctions)
//!< 		std::cout << "Cell_SPM::setVlimits starting with upper limit " << VMAX << " and lower limit " << VMIN << '\n';

//!< 	bool vmax = VMAX < 0 || VMAX > OCV_curves.OCV_pos.y[0];
//!< 	if (vmax)
//!< 		std::cerr << "ERROR in Cell_SPM::setVlimits. The value of the maximum voltage is " << VMAX
//!< 				  << "V but it has to be positive and lower than the maximum value of the OCV curve of the cathode, which is "
//!< 				  << OCV_curves.OCV_pos.y.back() << ".\n";
//!< 	bool vmin = VMIN < 0;
//!< 	if (vmin)
//!< 		std::cerr << "ERROR in Cell_SPM::setVlimits. The value of the minimum voltage is "
//!< 				  << VMIN << "V but it has to be positive.\n";

//!< 	if (vmax || vmin)
//!< 		throw 103;

//!< 	Vmax = VMAX;
//!< 	Vmin = VMIN;

//!< 	if constexpr (settings::printBool::printCellFunctions)
//!< 		std::cout << "Cell_SPM::setVlimits terminating with upper limit " << Vmax << " and lower limit " << Vmin << '\n';
//!< }

void Cell_SPM::setT(double Ti)
{
  /*
   * Set the temperature of the battery
   *
   * IN
   * Ti		uniform cell temperature, 273 <= T <= 333 [K]
   */

  st.T() = Ti; //!< #TODO if we need to check if we are in limits.  if T is in limits.

  //!< the stress values stored in the class variables for stress are no longer valid because the state has changed
  sparam.s_dai_update = false;
  sparam.s_lares_update = false;
}

void Cell_SPM::setC(double cp0, double cn0)
{
  /*
   * Function to set the value of the transformed concentrations to the values
   * corresponding with a uniform (user-specified) lithium concentration.
   * The cell's current is set to 0 because the concentration is fully uniform (which means the gradient at the surface is 0, so the current must be 0)
   *
   * IN
   * cp0	lithium fraction in the positive electrode 0 <= cp0 <= 1
   * cn0	lithium fraction in the negative electrode 0 <= cn0 <= 1
   *
   * THROWS
   * 104 	illegal input lithium fractions
   */
  //!< #NOTHOTFUNCTION
  using settings::nch;
  if constexpr (settings::printBool::printCellFunctions)
    std::cout << "Cell_SPM::setC starting.\n";

  bool pp = (cp0 < 0) || (cp0 > 1);
  if (pp)
    std::cerr << "ERROR in Cell_SPM::setC, illegal input li-fraction for the positive electrode : " << cp0
              << ". The value has to be between 0 and 1.\n";

  bool nn = (cn0 < 0) || (cn0 > 1);
  if (nn)
    std::cerr << "ERROR in Cell_SPM::setC, illegal input li-fraction for the negative electrode : " << cn0
              << ". The value has to be between 0 and 1.\n";
  if (pp || nn) {
    std::cout << "Throwed in File: " << __FILE__ << ", line: " << __LINE__ << '\n';
    throw 104;
  }

  //!< Calculate the corresponding li-concentrations in [mol m-3]
  const double cp = cp0 * Cmaxpos;
  const double cn = cn0 * Cmaxneg;

  //!< The second transformation is to the eigenspace: z = V * u with V the inverse of the matrix with the eigenvectors.
  //!< As explained, we know that there is one eigenvector corresponding to a uniform concentration
  //!< So we need to calculate only this one nonzero value for the (twice) transformed concentration
  //!< The location of the uniform eigenvector (which has a 0 eigenvalue) is written in M->Input[3]
  const auto ind = static_cast<size_t>(M->Input[3]);
  double znu = 0; //!< (twice) transformed negative uniform concentration
  double zpu = 0; //!< (twice) transformed positive uniform concentration

  for (size_t i = 0; i < nch; i++) {
    //!< Do the first transformation, to u(i) = radius(i) * concentration = x(i) * R * concentration(i)
    const auto uneg = cn * M->xch[i] * geo.Rn;
    const auto upos = cp * M->xch[i] * geo.Rp;
    //!< ---------------------------------------------------------------------------
    //!< loop to calculate the row of V * u corresponding to the uniform concentration
    znu += M->Vn[ind][i] * uneg;
    zpu += M->Vp[ind][i] * upos;

    st.zp(i) = 0; //!< Make the arrays for the (twice) transformed concentration with all zero
    st.zn(i) = 0; //!< except the ind which will be set soon.
  }

  st.zp(ind) = zpu;
  st.zn(ind) = znu;

  //!< Set the cell current to 0 to reflect the boundary condition for a fully uniform concentration
  st.I() = 0; //!< #TODO we do not do this.

  //!< the stress values stored in the class variables for stress are no longer valid because the state has changed
  sparam.s_dai_update = false;
  sparam.s_lares_update = false;

  if constexpr (settings::printBool::printCellFunctions)
    std::cout << "Cell_SPM::setC terminating.\n";
}

Cell_SPM::Cell_SPM(std::string IDi, const DEG_ID &degid, double capf, double resf, double degfsei, double degflam) : Cell_SPM()
{
  ID = IDi;

  //!< store such that the cell can later report its precise variation
  var_cap = capf;
  var_R = resf;
  var_degSEI = degfsei;
  var_degLAM = degflam;

  //!< Set the resistance

  st.rDCcc() *= resf; //!< current collector
  st.rDCp() *= resf;  //!< cathode
  st.rDCn() *= resf;  //!< anode

  //!< set the capacity
  setCapacity(Cap() * capf); //!< nominal capacity
  geo.elec_surf *= capf;     //!< surface area of the electrodes (current to current density)

  //!< set the degradation factor
  sei_p *= var_degSEI;
  csparam *= var_degSEI;
  lam_p *= var_degLAM;
  plparam *= var_degSEI;


  //!< set the degradation ID and related settings
  deg_id = degid;

  //!< Check if we will have to calculate the stress according to Dai's stress model
  for (auto cs_id : deg_id.CS_id)
    sparam.s_dai = sparam.s_dai || cs_id == 2;

  for (auto lam_id : deg_id.LAM_id)
    sparam.s_dai = sparam.s_dai || lam_id == 1;

  //!< check if we need to calculate the stress according to Laresgoiti's stress model
  for (auto cs_id : deg_id.CS_id)
    sparam.s_lares = sparam.s_lares || cs_id == 1;
}

//!< void Cell_SPM::setCurrent(bool print, bool check, double I)
//!< {
//!< 	/*
//!< 	 * Function to set the cell current to the specified value.
//!< 	 * The current is slowly ramped from the actual cell current to the specified value.
//!< 	 *
//!< 	 * IN
//!< 	 * print 	boolean indicating if we want to print error messages or not
//!< 	 * 				if true, error messages are printed
//!< 	 * 				if false no error messages are printed (but the error will still be thrown)
//!< 	 * 			we need this input from higher level functions because at this point we cannot know if this will be a critical error or not
//!< 	 * check 	if true, a check is done to see if the battery state at the end is valid or not (an error is thrown if not)
//!< 	 * 			if false, the current is just set without checking if the state is valid or not
//!< 	 * I 		value to which the current should be set [A]
//!< 	 * 			> 0 for discharge
//!< 	 * 			< 0 for charge
//!< 	 *
//!< 	 * THROWS
//!< 	 * 105 		check == true and the specified current could not be set without violating the cell's limits.
//!< 	 * 			the original battery state and current are restored
//!< 	 */
//!< 	//!< #HOTFUNC -> ~500k calls over 20M.
//!< 	if constexpr (settings::printBool::printCellFunctions)
//!< 		std::cout << "Cell_SPM::setCurrent starting with current " << I << '\n';

//!< 	//!< check if the specified value is different from the actual cell current
//!< 	if (std::abs(Icell - I) < 1e-6)
//!< 	{
//!< 		//!< No integration if the current is close enough.
//!< 		Icell = I;
//!< 		return;
//!< 	}

//!< 	//!< Store the old current and state to restore it if needed
//!< 	const auto sold = getStates();
//!< 	const auto Iold = I();

//!< 	//!< settings
//!< 	bool blockDegradation = true; //!< don't account for degradation while the current is ramping
//!< 	bool reached = false;		  //!< boolean indicating if we have reached the set current
//!< 	int sgn;					  //!< sgn whether we should increase or decrease the current
//!< 	if (I > Icell)
//!< 		sgn = 1;
//!< 	else
//!< 		sgn = -1;

//!< 	//!< loop to ramp the current
//!< 	while (!reached)
//!< 	{
//!< 		//!< increase the current
//!< 		Icell += sgn * dIcell;

//!< 		//!< check if you have reached the specified current
//!< 		if (sgn * Icell > sgn * I)
//!< 		{ //!< increase I: Icell > I, decrease I: Icell < I
//!< 			Icell = I;
//!< 			reached = true;
//!< 		}

//!< 		//!< take one small time step
//!< 		integratorStep(print, dt_I, blockDegradation);
//!< 	}

//!< 	//!< Check the cell's conditions are still valid if we want to check the final state
//!< 	double v, ocvp, ocvn, etap, etan, rdrop, tem;
//!< 	bool valid;
//!< 	if (check)
//!< 	{
//!< 		//!< check the state
//!< 		try
//!< 		{
//!< 			validState(); //!< throws an error if the state is illegal
//!< 		}
//!< 		catch (int e)
//!< 		{
//!< 			//!< std::cout << "Throw test: " << 38 << '\n';
//!< 			if (print)
//!< 				std::cout << "Cell_SPM::setCurrent illegal state after setting the current to "
//!< 						  << I() << ", error: " << e << ". Throwing an error.\n";
//!< 			setStates(sold, Iold); //!< restore the original battery state and current
//!< 			throw e;
//!< 		}

//!< 		//!< check the voltage

//!< 		const int status = getVoltage_ne(print, &v, &ocvp, &ocvn, &etap, &etan, &rdrop, &tem);
//!< 		if (status != 0)
//!< 		{
//!< 			if (print)
//!< 				std::cerr << "Cell_SPM::setCurrent Illegal voltage after trying to set the current to "
//!< 						  << I() << ", the voltage is: " << v << "V. Throwing an error.\n";
//!< 			setStates(sold, Iold); //!< restore the original battery state and current
//!< 			throw 105;
//!< 		}
//!< 	}

//!< 	//!< the stress values stored in the class variables for stress are no longer valid because the cell current has changed
//!< 	sparam.s_dai_update = false;
//!< 	sparam.s_lares_update = false;

//!< 	if constexpr (settings::printBool::printCellFunctions)
//!< 	{
//!< 		if (check)
//!< 			std::cout << "Cell_SPM::setCurrent terminating with current " << I << " and voltage " << v << '\n';
//!< 		else
//!< 			std::cout << "Cell_SPM::setCurrent terminating with current " << I << " without checking the voltage.\n";
//!< 	}
//!< }

//!< 	bool Cell_SPM::validStates(bool print)
//!< 	{
//!< 		/*
//!< 		 * Check the array contains states which are valid for an SPM
//!< 		 * This function checks
//!< 		 * 		the length of the array
//!< 		 * 		0 < SoC < 1, but does not check SoC is compatible with the concentration
//!< 		 * 		//todo calculate SoC based on the uniform concentration
//!< 		 * 		Tmin < T < Tmax
//!< 		 * 		0 < surface concentration < maximum concentration
//!< 		 * 		a = 3e/R
//!< 		 * 		all other values (except I) > 0
//!< 		 */
//!< #if TIMING
//!< 		std::clock_t tstart = std::clock();
//!< #endif

//!< 		//!< There are no limits on the transformed concentration, because this is the (twice) transformed concentration.
//!< 		//!< The limits are on the real concentrations, which must be calculated first.
//!< 		//!< This can't be done here because it requires parameters of the Cell itself.
//!< 		//!< see Cell_SPM::getC or Cell_SPM::getCsurf

//!< 		constexpr double tol = 1e-5;

//!< 		const bool verb = settings::printBool::printCrit && print; //!< print if the (global) verbose-setting is above the threshold
//!< 		bool range = true;										   //!< are all in the allowed range?

//!< 		if (!checkSOC(st.SOC())) //!< check whether SoC in the allowed range
//!< 		{
//!< 			if (verb)
//!< 				std::cerr << "ERROR in Cell_SPM::validState, SoC is outside of the range, 0 <= SoC <= 1: "
//!< 						  << st.SOC() << '\n';
//!< 			range = false;
//!< 		}

//!< 		//!< temperature of the cell (in Kelvin), the temperature limits are defined in State.hpp
//!< 		if (st.T() < Tmin()) //!< check the temperature is above 0 degrees
//!< 		{
//!< 			std::cerr << "Error in Cell_SPM::validState. The temperature " << st.T()
//!< 					  << "K is too low. The minimum value is " << settings::Tmin_K << '\n';
//!< 			range = false;
//!< 		}
//!< 		else if (st.T() > Tmax()) //!< check the temperature is below 60 degrees
//!< 		{
//!< 			std::cerr << "Error in Cell_SPM::validState. The temperature " << st.T()
//!< 					  << "K is too high. The maximum value is " << settings::Tmax_K << '\n';
//!< 			range = false;
//!< 		}

//!< 		//!< thickness of the SEI layer
//!< 		const bool del = st.delta() <= 0;
//!< 		if (del)
//!< 		{
//!< 			std::cerr << "Error in State::validState. The SEI thickness delta is " << st.delta()
//!< 					  << ", which is too low. Only strictly positive values are allowed.\n";
//!< 			throw 15;
//!< 		}
//!< 		//!< a value of 0 gives problems in some equations, which have a term 1/delta, which would give nan or inf if delta = 0
//!< 		//!< a negative value might lead to a further decrease in SEI thickness, so it will keep getting more and more negative

//!< 		//!< //!< lost lithium
//!< 		//!< bool li = st.LLI() < 0;
//!< 		//!< if (li)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The lost lithium LLI is " << st.LLI()
//!< 		//!< 			  << ", which is too low. Only non-negative values are allowed.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }
//!< 		//!< //!< a value of 0 is allowed (it won't lead to errors in the code)

//!< 		//!< //!< thickness of the electrodes
//!< 		//!< bool tpmin = st.thickp() <= s_ini.thickp() / 5;
//!< 		//!< if (tpmin)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the thickness of the positive electrode is " << st.thickp() << ", which is too low. The minimum is " << s_ini.thickp() / 5 << ", 1/5 of the original thickness"
//!< 		//!< 			  << '\n';
//!< 		//!< 	throw 15;
//!< 		//!< }
//!< 		//!< //!< errors will happen if the thickness becomes 0 or negative. otherwise no direct problems are expected
//!< 		//!< //!< on a longer term, you will get discretisation errors (see above, too little active material -> too large concentration swings in one time step -> numerical errors / concentration out of bound, etc
//!< 		//!< bool tpmax = st.thickp() > s_ini.thickp() * 1.001; //!< leave a 0.1% margin for numerical errors
//!< 		//!< if (tpmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The thickness of the positive electrode is " << st.thickp()
//!< 		//!< 			  << ", which is too high. The maximum is " << s_ini.thickp() << ", the original thickness\n";
//!< 		//!< 	throw 15;
//!< 		//!< }
//!< 		//!< bool tnmin = st.thickn() <= s_ini.thickn() / 5;
//!< 		//!< if (tnmin)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the thickness of the negative electrode is " << st.thickn()
//!< 		//!< 			  << ", which is too low. The minimum is " << s_ini.thickn() / 5 << ", 1/5 of the original thickness\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< //!< errors will happen if the thickness becomes 0 or negative. otherwise no direct problems are expected
//!< 		//!< //!< on a longer term, you will get discretisation errors (see above, too little active material -> too large concentration swings in one time step -> numerical errors / concentration out of bound, etc
//!< 		//!< bool tnmax = st.thickn() > s_ini.thickn() * 1.001;
//!< 		//!< if (tnmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The thickness of the negative electrode is " << st.thickn()
//!< 		//!< 			  << ", which is too high. The maximum is " << s_ini.thickn() << ", the original thickness.\n";

//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< //!< volume fraction of the active material in the electrodes
//!< 		//!< bool epmin = st.ep() <= s_ini.ep() / 5;
//!< 		//!< if (epmin)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the volume fraction of the positive electrode is "
//!< 		//!< 			  << st.ep() << ", which is too low. The minimum is " << s_ini.ep() / 5 << ", 1/5 of the original volume fraction.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< //!< errors will happen if the volume fraction becomes 0 or negative. otherwise no direct problems are expected
//!< 		//!< //!< on a longer term, you will get discretisation errors (see above, too little active material -> too large concentration swings in one time step -> numerical errors / concentration out of bound, etc
//!< 		//!< bool epmax = st.ep() > s_ini.ep() * 1.001;
//!< 		//!< if (epmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The volume fraction of the positive electrode is " << st.ep()
//!< 		//!< 			  << ", which is too high. The maximum is " << s_ini.ep() << ", the original volume fraction.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< bool enmin = st.en() <= s_ini.en() / 5;
//!< 		//!< if (enmin)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the volume fraction of the negative electrode is " << st.en()
//!< 		//!< 			  << ", which is too low. The minimum is " << s_ini.en() / 5 << ", 1/5 of the original volume fraction.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< //!< errors will happen if the volume fraction becomes 0 or negative. otherwise no direct problems are expected
//!< 		//!< //!< on a longer term, you will get discretisation errors (see above, too little active material -> too large concentration swings in one time step -> numerical errors / concentration out of bound, etc
//!< 		//!< bool enmax = st.en() > s_ini.en() * 1.001;
//!< 		//!< if (enmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The volume fraction of the negative electrode is " << st.en()
//!< 		//!< 			  << ", which is too high. The maximum is " << s_ini.en() << ", the original volume fraction.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< //!< effective surface area of the porous electrodes
//!< 		//!< bool apmin = st.ap() <= s_ini.ap() / 5;
//!< 		//!< if (apmin)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the effective surface of the positive electrode is " << st.ap()
//!< 		//!< 			  << ", which is too low. The minimum is " << s_ini.ap() / 5 << ", 1/5 of the original effective surface.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }
//!< 		//!< //!< errors will happen if the effective surface becomes 0 or negative. otherwise no direct problems are expected
//!< 		//!< //!< on a longer term, you will get discretisation errors (see above, too little active material -> too large concentration swings in one time step -> numerical errors / concentration out of bound, etc
//!< 		//!< bool apmax = st.ap() > s_ini.ap() * 1.001;
//!< 		//!< if (apmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The effective surface of the positive electrode is " << st.ap()
//!< 		//!< 			  << ", which is too high. The maximum is " << s_ini.ap() << ", the original effective surface.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< bool anmin = st.an() <= s_ini.an() / 5;
//!< 		//!< if (anmin)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the effective surface of the negative electrode is " << st.an()
//!< 		//!< 			  << ", which is too low. The minimum is " << s_ini.an() / 5 << ", 1/5 of the original effective surface.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< //!< errors will happen if the effective surface becomes 0 or negative. otherwise no direct problems are expected
//!< 		//!< //!< on a longer term, you will get discretisation errors (see above, too little active material -> too large concentration swings in one time step -> numerical errors / concentration out of bound, etc
//!< 		//!< bool anmax = st.an() > s_ini.an() * 1.001;
//!< 		//!< if (anmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The effective surface of the negative electrode is " << st.an()
//!< 		//!< 			  << ", which is too high. The maximum is " << s_ini.an() << ", the original effective surface.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< surface area of the cracks growing at the particle surface
//!< 		const bool csmin = st.CS() <= 0;
//!< 		if (csmin)
//!< 		{
//!< 			std::cerr << "Error in State::validState. The crack surface area is " << st.CS()
//!< 					  << ", which is too low. It must be strictly positive.\n";
//!< 			range = false;
//!< 		}

//!< 		//!< //!< don't allow 0 because it might give problems in some of the equations, which might grow CS proportional to the existing CS (so a value of 0 gives 0 increase)
//!< 		//!< //!< a negative value might lead to a further decrease in CS, so it will keep getting more and more negative
//!< 		//!< bool csmax = st.CS() > 1e4 * s_ini.CS();
//!< 		//!< if (csmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the crack surface area is "
//!< 		//!< 			  << st.CS() << ", which is too high. The maximum is " << 1e4 * s_ini.CS()
//!< 		//!< 			  << ", 10,000 times the original crack surface area.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }
//!< 		//!< normally, the initial value is 1% of the total real electrode surface area, so 10,000*initial value = 100 * total electrode surface area
//!< 		//!< but in theory no maximum value will give errors in the code

//!< 		//!< //!< diffusion constant at reference temperature for the electrodes
//!< 		//!< bool dpmin = st.Dp() <= s_ini.Dp() / 5;
//!< 		//!< if (dpmin)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the diffusion constant of the positive electrode is " << st.Dp()
//!< 		//!< 			  << ", which is too low. The minimum is " << s_ini.Dp() / 5 << ", 1/5 of the original diffusion constant.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< //!< errors will happen if the diffusion constant becomes 0 or negative. otherwise no direct problems are expected
//!< 		//!< //!< on a longer term, you will get discretisation errors (see above, too bad diffusion -> too large surface concentration swings in one time step -> numerical errors / concentration out of bound, etc
//!< 		//!< bool dpmax = st.Dp() > s_ini.Dp() * 1.001;
//!< 		//!< if (dpmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The diffusion constant of the positive electrode is "
//!< 		//!< 			  << st.Dp() << ", which is too high. The maximum is "
//!< 		//!< 			  << s_ini.Dp() << ", the original diffusion constant.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< bool dnmin = st.Dn() <= s_ini.Dn() / 5;
//!< 		//!< if (dnmin)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the diffusion constant of the negative electrode is " << st.Dn()
//!< 		//!< 			  << ", which is too low. The minimum is " << s_ini.Dn() / 5 << ", 1/5 of the original diffusion constant.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }
//!< 		//!< //!< errors will happen if the diffusion constant becomes 0 or negative. otherwise no direct problems are expected
//!< 		//!< //!< on a longer term, you will get discretisation errors (see above, too bad diffusion -> too large surface concentration swings in one time step -> numerical errors / concentration out of bound, etc
//!< 		//!< bool dnmax = st.Dn() > s_ini.Dn() * 1.001;
//!< 		//!< if (dnmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The diffusion constant of the negative electrode is "
//!< 		//!< 			  << st.Dn() << ", which is too high. The maximum is "
//!< 		//!< 			  << s_ini.Dn() << ", the original effective diffusion constant.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< specific resistance of the electrodes (one value for both electrodes combined)
//!< 		bool rmin = st.rDCp() < 0 || st.rDCn() < 0 || st.rDCcc() < 0;
//!< 		if (rmin)
//!< 		{
//!< 			std::cerr << "Error in State::validState. The specific resistance is "
//!< 					  << "too low, it must be strictly positive.\n";
//!< 			range = false;
//!< 		}

//!< 		//!< bool rmax = st.r() > 1000 * s_ini.r();
//!< 		//!< if (rmax)
//!< 		//!< {
//!< 		//!< 	std::cerr << "Error in State::validState. The cell has degraded too much and the specific resistance is " << st.r()
//!< 		//!< 			  << ", which is too high. The maximum is " << 1000 * s_ini.r() << ", 1000 times the original specific resistance.\n";
//!< 		//!< 	throw 15;
//!< 		//!< }

//!< 		//!< thickness of the plated litium layer
//!< 		const bool delpl = st.delta_pl() < 0;
//!< 		if (delpl)
//!< 		{
//!< 			std::cerr << "Error in State::validState. The thickness of the plated lithium is " << st.delta_pl()
//!< 					  << ", which is too low. Only strictly positive values are allowed.\n";
//!< 			range = false;

//!< 			//!< 0 is allowed
//!< 			//!< a negative value might lead to a further decrease in thickness, so it will keep getting more and more negative
//!< 		}

//!< #if TIMING
//!< 		T_validStates += (std::clock() - tstart) / static_cast<double>(CLOCKS_PER_SEC); //!< time in seconds
//!< #endif

//!< 		return range;
//!< 	}

void Cell_SPM::checkModelparam()
{
  //!< check if the inputs to the MATLAB code are the same as the ones here in the C++ code
  //!< input:
  //!< 		M->Input[0] has to be the same as nch (defined in State.hpp)
  //!< 		M->Input[1] has to be the same as Rp (defined earlier in this constructor)
  //!< 		M->Input[2] has to be the same as Rn (defined earlier in this constructor)
  //!< 		M->input[3] has to give the location of the 0 eigenvalue

  using settings::nch;
  const bool Mnch = (M->Input[0] - nch) / M->Input[0] > 1e-10; //!< allow a relative difference of e-10 due to numerical errors
  if (Mnch)
    std::cerr << "ERROR in Cell_KokamNMC::Cell_KokamNMC: the value of nch used in the MATLAB script " << M->Input[0]
              << " is not the same as the value of nch used in the c++ code " << nch << ".\n";

  const bool Mrp = (M->Input[1] - geo.Rp) / M->Input[1] > 1e-10; //!< allow a relative difference of e-10 due to numerical errors
  if (Mrp)
    std::cerr << "ERROR in Cell_KokamNMC::Cell_KokamNMC: the value of Rp used in the MATLAB script " << M->Input[1]
              << " is not the same as the value of Rp used in the c++ code " << geo.Rp << ".\n";
  const bool Mrn = (M->Input[2] - geo.Rn) / M->Input[2] > 1e-10; //!< allow a relative difference of e-10 due to numerical errors
  if (Mrn)
    std::cerr << "ERROR in Cell_KokamNMC::Cell_KokamNMC: the value of Rn used in the MATLAB script " << M->Input[2]
              << " is not the same as the value of Rn used in the c++ code " << geo.Rn << ".\n";
  const auto a = static_cast<int>(M->Input[3]);
  const bool Meig = std::abs(M->An[a]) > 1e-10 || std::abs(M->Ap[a]) > 1e-10; //!< allow a relative difference of e-10 due to numerical errors
  if (Meig)
    std::cerr << "ERROR in Cell_KokamNMC::Cell_KokamNMC: the row of the 0-eigenvalue is " << M->Input[3]
              << " but that row has a positive eigenvalue of " << M->Ap[a] << " and negative eigenvalue of " << M->An[a] << ". They are not 0.\n";
  if (Mnch || Mrp || Mrn || Meig) {
    std::cout << "The MATLAB script modelSetup.m produces matrices used by the C++ code for the discretisation of the solid diffusion equation."
                 " MATLAB needs some input parameters, such as the number of nodes and the radius of the particles. These parameters are specified on top of the MATLAB scripts."
                 " These values are also defined in the C++ code. Of course, both values have to be the same."
                 " It turned out this was not the case, so you either have to change the values in the MATLAB script or the ones in the C++ code."
                 " We are throwing an error.\n";
    std::cout << "Throwed in File: " << __FILE__ << ", line: " << __LINE__ << '\n';
    throw 110;
  }
}
} // namespace slide