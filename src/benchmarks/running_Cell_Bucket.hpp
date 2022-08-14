/*
 * running_Cell_Bucket.hpp
 *
 *  Benchmark file for Cell_Bucket
 *
 *  Created on: 07 Aug 2022
 *   Author(s): Volkan Kumtepeli
 */

#pragma once

#include "../slide.hpp"

#include <string>

namespace slide::benchmarks {
inline void run_Cell_Bucket()
{
  std::string ID = "temp";
  Clock clk;

  constexpr size_t N = 10000;

  auto c = Cell_Bucket();
  auto cyc = Cycler(&c, ID);

  for (size_t i{ 0 }; i < N; i++) {
    double Ah, Wh, dtime;

    cyc.CCCV(1, 4, 0.1, 1, 0, Ah, Wh, dtime);
    cyc.CCCV(1, 3, 0.1, 1, 0, Ah, Wh, dtime);
  }

  std::cout << "V: " << c.V() << "\n";

  std::cout << "Finished run_Cell_Bucket in " << clk << ".\n";
};

inline void run_Cell_SPM()
{

  std::string ID = "temp";
  Clock clk;

  constexpr size_t N = 200;

  auto c = Cell_SPM();
  auto cyc = Cycler(&c, ID);

  for (size_t i{ 0 }; i < N; i++) {
    double Ah, Wh, dtime;

    cyc.CCCV(1, 4, 0.1, 1, 0, Ah, Wh, dtime);
    cyc.CCCV(1, 3, 0.1, 1, 0, Ah, Wh, dtime);
  }

  std::cout << "V: " << c.V() << "\n";
  std::cout << "Finished run_Cell_SPM in " << clk << ".\n";
};

} // namespace slide::benchmarks