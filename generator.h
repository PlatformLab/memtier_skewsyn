#ifndef _GENERATOR_H
#define _GENERATOR_H

#include <random>
#include <string>

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum DistributionType { POISSON = 0, UNIFORM };
extern std::random_device rd;

// Generator types are based on distribution type
// Basic class will always return 0
// Now we support Poisson and Uniform, may support fixed
class Generator {
  public:
    Generator() {}
    virtual ~Generator() {}

    virtual double generate() { return 0.0; }
    virtual void set_lambda(double lambda) = 0;
    virtual double get_lambda() = 0;
};

// Poisson distribution, lambda is creations per second
class Poisson : public Generator {
  public:
    Poisson(double _lambda = 1.0)
        : lambda(_lambda), gen(rd()), expIG(_lambda) {}

    virtual double generate() override {
        if (this->lambda <= 0.0)
            return 0.0;
        return this->expIG(gen);
    }

    virtual void set_lambda(double lambda) override {
        if (this->lambda == lambda)
            return;
        this->lambda = lambda;
        if (lambda > 0.0) {
            this->expIG.param(
                std::exponential_distribution<double>::param_type(lambda));
        }
    }

    virtual double get_lambda() override { return this->lambda; }

  private:
    double lambda;
    std::mt19937 gen;
    std::exponential_distribution<double> expIG;
};

// Uniform distribution, lambda is creations per second
// We want the average value to be consistent with the expectation
// So we need to use 2.0 instead of 1.0 when setting max value.
class Uniform : public Generator {
  public:
    Uniform(double _lambda = 1.0)
        : lambda(_lambda), gen(rd()), uniformIG(0, 2.0 / _lambda) {}

    virtual double generate() override {
        if (this->lambda <= 0.0)
            return 0.0;
        return this->uniformIG(gen);
    }

    virtual void set_lambda(double lambda) override {
        if (this->lambda == lambda)
            return;
        this->lambda = lambda;
        if (lambda > 0.0) {
            this->uniformIG.param(
                std::uniform_real_distribution<double>::param_type(
                    0, 2.0 / lambda));
        }
    }

    virtual double get_lambda() override { return this->lambda; }

  private:
    double lambda;
    std::mt19937 gen;
    std::uniform_real_distribution<double> uniformIG;
};

#endif  // _GENERATOR_H
