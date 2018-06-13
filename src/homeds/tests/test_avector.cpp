//
// Created by Kadayam, Hari on 30/11/17.
//

#include <iostream>
#include <sds_logging/logging.h>
#include "homeds/array/sparse_vector.hpp"

SDS_LOGGING_INIT

void func(const homeds::sparse_vector<int> &cvec) {
    std::cout << "vec.at(1) = " << cvec.at(1) << "\n";
    std::cout << "vec.at(7) = " << cvec.at(7) << "\n";
    std::cout << "vec[7] = " << cvec[7] << " Vector size = " << cvec.size() << "\n";
}

int main(int argc, char *argv[]) {
    homeds::sparse_vector<int> vec;
    vec.reserve(10);

    vec.push_back(1);
    vec.push_back(2);
    std::cout << "Vector size = " << vec.size() << "\n";
    vec[5] = 6;
    vec.at(6) = 7;
    std::cout << "vec[5] = " << vec[5] << " vec.at(6) = " << vec.at(6) << " Vector size = " << vec.size() << "\n";
    func(vec);
}
