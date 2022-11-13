/*
 * Battery_test.cpp
 *
 *  Created on: 11 Jun 2020
 *   Author(s): Jorn Reniers, Volkan Kumtepeli
 */

#include "Battery.hpp"

#include "Cell_SPM.hpp"
#include "unit_tests.hpp"
#include "constants.hpp"
#include "Module_s.hpp"
#include "Module_p.hpp"
#include "Cycler.hpp"

#include <cmath>
#include <random>
#include <cassert>
#include <iostream>
#include <fstream>

void test_Battery_CoolSystem()
{
  /*
   * test the cool system design with proper cycle ageing
   */

  //!< Parameters from Cell_SPM which are needed to calculate the heat balance
  double rho = 1626;
  double Cp = 750;
  double L = 1.6850 * 1e-4;           //!< thickness of one layer
  double width = 0.1;                 //!< width of the pouch
  double height = 0.2;                //!< height of the pouch
  int nlayers = 31;                   //!< number of layers in the pouch
  double Acell = width * height;      //!< geometric surface area of the pouch
  double elec_surf = Acell * nlayers; //!< total 'unrolled' surface area of the electrodes

  //!< General settings
  double T = settings::T_ENV;
  bool checkCells = true;
  double Icha, Idis;
  double dt = 2;
  int N = 10;
  Cycler cyc;
  double lim = 0.0;
  double Ah, Wh;
  double vlim, tlim;
  int ndata = 0;

  //!< Loop for each setting of the cool controller
  for (int coolControl = 1; coolControl < 6; coolControl++) {

    //!< ****************************************************************************************************************************************************
    //!< Make a simple module with one SPM cell
    int ncel = 1;
    auto cp0 = std::make_unique<Cell_SPM>();
    std::unique_ptr<StorageUnit> cs[] = { cp0 };
    std::string n = "testCoolSystem";
    auto mp = std::make_unique<Module_s>(n, T, true, false, ncel, coolControl, 2); //!< open coolsystem
    mp->setSUs(cs, checkCells, true);
    double Tini[1] = { cp0->T() };
    auto b1 = std::make_unique<Battery>(n);
    b1->setModule(mp);
    cyc.initialise(b1, "Cycler_cooltest_oneCell");

    //!< do a few 1C cycles
    Icha = -cp0->Cap();
    Idis = cp0->Cap();
    for (int i = 0; i < N; i++) {
      //!< charge
      vlim = mp->Vmax() - lim;
      tlim = 99999999;
      cyc.CC(Icha, vlim, tlim, dt, ndata, Ah, Wh);

      //!< CC discharge
      vlim = mp->Vmin() + lim;
      cyc.CC(Idis, vlim, tlim, dt, ndata, Ah, Wh);
    }

    //!< check the energy balance of the outer module
    double Qgen = cp0->thermal_getTotalHeat();         //!< total heat generated by cells
    double Qcool = mp->getCoolSystem()->getHeatEvac(); //!< total heat extracted by the coolsystem from the cells
    double Tnew[1] = { cp0->T() };
    double Qheat = 0; //!< total energy in heating up the cells
    for (int i = 0; i < 1; i++)
      Qheat += (Tnew[i] - Tini[i]) * (rho * Cp * L * elec_surf);
    //!< cout<<"Total heat balance of coolsystem single cell "<<coolControl<<" is "<<Qgen<<", "<<Qheat<<", "<<Qcool<<" and error "<<abs(Qgen - Qcool - Qheat)<<endl;
    assert(std::abs(Qgen - Qcool - Qheat) / std::abs(Qgen) < 1e-10);

    //!< check energy balance of the battery
    Qgen += b1->getAndResetConvLosses();
    Qcool = b1->getCoolSystem()->getQcoolAC_tot();                                            //!< total evacuated by AC system
    Qheat += mp->getCoolSystem()->getHeatabsorbed() + b1->getCoolSystem()->getHeatabsorbed(); //!< include heating of the battery since heatAC is total
    //!< cout<<"\t Total battery balance for coolcontrol"<<coolControl<<" is "<<Qgen<<", "<<Qheat<<", "<<Qcool<<" and error "<<abs(Qgen - Qcool - Qheat)<<endl;
    assert(std::abs(Qgen - Qcool - Qheat) / std::abs(Qgen) < 1e-10);

    //!< **********************************************************************************************************************************************************
    //!< Make a simple module with SPM cells
    int ncel2 = 4;
    auto cp1 = std::make_unique<Cell_SPM>();
    auto cp2 = std::make_unique<Cell_SPM>();
    auto cp3 = std::make_unique<Cell_SPM>();
    auto cp4 = std::make_unique<Cell_SPM>();
    std::unique_ptr<StorageUnit> cs2[] = { cp1, cp2, cp3, cp4 };
    std::string n2 = "testCoolSystem";
    auto mp2 = std::make_unique<Module_s>(n2, T, true, false, ncel2, coolControl, 2); //!< open coolsystem
    mp2->setSUs(cs2, checkCells, true);
    double Tini2[4] = { cp1->T(), cp2->T(), cp3->T(), cp4->T() };
    auto b2 = std::make_unique<Battery>(n2);
    b2->setModule(mp2);
    cyc.initialise(b2, "Cycler_cooltest_simpleModule");

    //!< do a few 1C cycles (note just some time steps since we don't have the Cycler
    Icha = -cp1->Cap();
    Idis = cp1->Cap();
    for (int i = 0; i < 5; i++) {
      //!< charge
      vlim = mp2->Vmax() - lim;
      tlim = 99999999;
      cyc.CC(Icha, vlim, tlim, dt, ndata, Ah, Wh);

      //!< CC discharge
      vlim = mp2->Vmin() + lim;
      cyc.CC(Idis, vlim, tlim, dt, ndata, Ah, Wh);
    }

    //!< check the energy balance of the outer module
    double Qgen2 = cp1->thermal_getTotalHeat() + cp2->thermal_getTotalHeat() + cp3->thermal_getTotalHeat() + cp4->thermal_getTotalHeat(); //!< total heat generated by cells
    double Qcool2 = mp2->getCoolSystem()->getHeatEvac();                                                                                  //!< total heat extracted by the coolsystem from the cells
    double Tnew2[4] = { cp1->T(), cp2->T(), cp3->T(), cp4->T() };
    double Qheat2 = 0; //!< total energy in heating up the cells
    for (int i = 0; i < 4; i++)
      Qheat2 += (Tnew2[i] - Tini2[i]) * (rho * Cp * L * elec_surf);
    //!< cout<<"Total heat balance of coolsystem simle module "<<coolControl<<" is "<<Qgen2<<", "<<Qheat2<<", "<<Qcool2<<" and error "<<abs(Qgen2 - Qcool2 - Qheat2)<<endl;
    assert(std::abs(Qgen2 - Qcool2 - Qheat2) / std::abs(Qgen2) < 1e-10);

    //!< check energy balance of the battery
    Qgen2 += b2->getAndResetConvLosses();
    Qcool2 = b2->getCoolSystem()->getQcoolAC_tot();
    Qheat2 += mp2->getCoolSystem()->getHeatabsorbed() + b2->getCoolSystem()->getHeatabsorbed();
    //!< cout<<"\t Total battery balance for coolcontrol"<<coolControl<<" is "<<Qgen2<<", "<<Qheat2<<", "<<Qcool2<<" and error "<<abs(Qgen2 - Qcool2 - Qheat2)<<endl;
    assert(std::abs(Qgen2 - Qcool2 - Qheat2) / std::abs(Qgen2) < 1e-10);

    //!< ******************************************************************************************************************************************************
    //!< make the hierarchical module
    int ncel11 = 2;
    int ncel22 = 2;
    int ncel33 = 3;
    std::string n11 = "H1";
    std::string n22 = "H2";
    std::string n33 = "H3";
    auto cp11 = std::make_unique<Cell_SPM>();
    auto cp22 = std::make_unique<Cell_SPM>();
    auto cp33 = std::make_unique<Cell_SPM>();
    auto cp44 = std::make_unique<Cell_SPM>();
    auto cp55 = std::make_unique<Cell_SPM>();
    auto cp66 = std::make_unique<Cell_SPM>();
    auto cp77 = std::make_unique<Cell_SPM>();
    std::unique_ptr<StorageUnit> SU1[] = { cp11, cp22 };
    std::unique_ptr<StorageUnit> SU2[] = { cp33, cp44 };
    std::unique_ptr<StorageUnit> SU3[] = { cp55, cp66, cp77 };
    auto mp11 = std::make_unique<Module_s>(n11, T, true, false, ncel11, coolControl, 0); //!< normal coolsystem (with fan)
    auto mp22 = std::make_unique<Module_s>(n22, T, true, false, ncel22, coolControl, 0);
    auto mp33 = std::make_unique<Module_s>(n33, T, true, false, ncel33, coolControl, 0);
    mp11->setSUs(SU1, ncel11, checkCells);
    mp22->setSUs(SU2, ncel22, checkCells);
    mp33->setSUs(SU3, ncel33, checkCells);
    int nm = 3;
    std::string n44 = "H4";
    std::unique_ptr<StorageUnit> MU[] = { mp11, mp22, mp33 };
    auto mp44 = std::make_unique<Module_s>(n44, T, true, true, 7, coolControl, 2); //!< open coolsystem
    mp44->setSUs(MU, checkCells, true);
    double Tini22[7] = { cp11->T(), cp22->T(), cp33->T(), cp44->T(), cp55->T(), cp66->T(), cp77->T() };
    auto b3 = std::make_unique<Battery>(n44);
    b3->setModule(mp44);
    cyc.initialise(b3, "Cycler_cooltest_complexModule");

    //!< do a few 1C cycles (note just some time steps since we don't have the Cycler
    Icha = -cp11->Cap();
    Idis = cp11->Cap();
    for (int i = 0; i < 5; i++) {
      //!< charge
      vlim = mp44->Vmax() - lim;
      tlim = 99999999;
      cyc.CC(Icha, vlim, tlim, dt, ndata, Ah, Wh);

      //!< CC discharge
      vlim = mp44->Vmin() + lim;
      cyc.CC(Idis, vlim, tlim, dt, ndata, Ah, Wh);
    }

    double Qgen3, Qcool3, Qheat3;
    //!< check balance of module mp11
    Qgen3 = cp11->thermal_getTotalHeat() + cp22->thermal_getTotalHeat();                                                     //!< total heat generated by cells
    Qcool3 = mp11->getCoolSystem()->getHeatEvac();                                                                           //!< total heat extracted by the coolsystem from the cells
    Qheat3 = -((Tini22[0] - cp11->T()) * (rho * Cp * L * elec_surf) + (Tini22[1] - cp22->T()) * (rho * Cp * L * elec_surf)); //!< total energy in heating up the cells
    //!< cout<<"Total heat balance of coolsystem complex module 1 "<<coolControl<<" is "<<Qgen3<<", "<<Qheat3<<", "<<Qcool3<<" and error "<<abs(Qgen3 - Qcool3 - Qheat3)<<endl;
    assert(std::abs(Qgen3 - Qcool3 - Qheat3) / std::abs(Qgen3) < 1e-10);
    //!< check balance of module mp22
    Qgen3 = cp33->thermal_getTotalHeat() + cp44->thermal_getTotalHeat();                                                     //!< total heat generated by cells
    Qcool3 = mp22->getCoolSystem()->getHeatEvac();                                                                           //!< total heat extracted by the coolsystem from the cells
    Qheat3 = -((Tini22[2] - cp33->T()) * (rho * Cp * L * elec_surf) + (Tini22[3] - cp44->T()) * (rho * Cp * L * elec_surf)); //!< total energy in heating up the cells
    //!< cout<<"Total heat balance of coolsystem complex module 2 "<<coolControl<<" is "<<Qgen3<<", "<<Qheat3<<", "<<Qcool3<<" and error "<<abs(Qgen3 - Qcool3 - Qheat3)<<endl;
    assert(std::abs(Qgen3 - Qcool3 - Qheat3) / std::abs(Qgen3) < 1e-10);
    //!< check balance of module mp33
    Qgen3 = cp55->thermal_getTotalHeat() + cp66->thermal_getTotalHeat() + cp77->thermal_getTotalHeat();                                                                             //!< total heat generated by cells
    Qcool3 = mp33->getCoolSystem()->getHeatEvac();                                                                                                                                  //!< total heat extracted by the coolsystem from the cells
    Qheat3 = -((Tini22[4] - cp55->T()) * (rho * Cp * L * elec_surf) + (Tini22[5] - cp66->T()) * (rho * Cp * L * elec_surf) + (Tini22[6] - cp77->T()) * (rho * Cp * L * elec_surf)); //!< total energy in heating up the cells
    //!< cout<<"Total heat balance of coolsystem complex module 3 "<<coolControl<<" is "<<Qgen3<<", "<<Qheat3<<", "<<Qcool3<<" and error "<<abs(Qgen3 - Qcool3 - Qheat3)<<endl;
    assert(std::abs(Qgen3 - Qcool3 - Qheat3) / std::abs(Qgen3) < 1e-10);

    //!< check balance of the top level module
    Qgen3 = mp11->getCoolSystem()->getHeatEvac() + mp22->getCoolSystem()->getHeatEvac() + mp33->getCoolSystem()->getHeatEvac();
    Qcool3 = mp44->getCoolSystem()->getHeatEvac();
    Qheat3 = mp11->getCoolSystem()->getHeatabsorbed() + mp22->getCoolSystem()->getHeatabsorbed() + mp33->getCoolSystem()->getHeatabsorbed();
    //!< cout<<"Total heat balance of coolsystem complex module top "<<coolControl<<" is "<<Qgen3<<", "<<Qheat3<<", "<<Qcool3<<" and error "<<abs(Qgen3 - Qcool3 - Qheat3)<<endl;
    assert(std::abs(Qgen3 - Qcool3 - Qheat3) / std::abs(Qgen3) < 1e-10);

    //!< check balance of total system
    Qgen3 = cp11->thermal_getTotalHeat() + cp22->thermal_getTotalHeat();
    Qgen3 += cp33->thermal_getTotalHeat() + cp44->thermal_getTotalHeat();
    Qgen3 += cp55->thermal_getTotalHeat() + cp66->thermal_getTotalHeat() + cp77->thermal_getTotalHeat();
    Qcool3 = mp44->getCoolSystem()->getHeatEvac();
    Qheat3 = -((Tini22[0] - cp11->T()) * (rho * Cp * L * elec_surf) + (Tini22[1] - cp22->T()) * (rho * Cp * L * elec_surf));
    Qheat3 += -((Tini22[2] - cp33->T()) * (rho * Cp * L * elec_surf) + (Tini22[3] - cp44->T()) * (rho * Cp * L * elec_surf));
    Qheat3 += -((Tini22[4] - cp55->T()) * (rho * Cp * L * elec_surf) + (Tini22[5] - cp66->T()) * (rho * Cp * L * elec_surf) + (Tini22[6] - cp77->T()) * (rho * Cp * L * elec_surf));
    Qheat3 += mp11->getCoolSystem()->getHeatabsorbed() + mp22->getCoolSystem()->getHeatabsorbed() + mp33->getCoolSystem()->getHeatabsorbed();
    assert(std::abs(Qgen3 - Qcool3 - Qheat3) / std::abs(Qgen3) < 1e-10);
    assert(Qheat3 > 0);

    //!< Comparison of cool system performance in the different control strategies: print out the following statement
    //!< cout<<"Total heat balance of coolsystem complex module entire "<<coolControl<<" is "<<Qgen3<<", "<<Qheat3<<", "<<Qcool3<<" and error "<<abs(Qgen3 - Qcool3 - Qheat3)<<endl;

    //!< check energy balance of the battery
    Qgen3 += b3->getAndResetConvLosses();
    Qcool3 = b3->getCoolSystem()->getQcoolAC_tot();
    Qheat3 += mp44->getCoolSystem()->getHeatabsorbed() + b3->getCoolSystem()->getHeatabsorbed();
    //!< cout<<"\t Total battery balance for coolcontrol"<<coolControl<<" is "<<Qgen3<<", "<<Qheat3<<", "<<Qcool3<<" and error "<<abs(Qgen3 - Qcool3 - Qheat3)<<endl;
    assert(std::abs(Qgen3 - Qcool3 - Qheat3) / std::abs(Qgen3) < 1e-10);
  }
}

void test_Battery()
{
  test_Battery_CoolSystem();
}
