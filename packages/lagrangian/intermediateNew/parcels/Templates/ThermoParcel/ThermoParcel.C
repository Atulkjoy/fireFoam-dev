/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2017 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "ThermoParcel.H"
#include "physicoChemicalConstants.H"

using namespace Foam::constant;

// * * * * * * * * * * *  Protected Member Functions * * * * * * * * * * * * //

template<class ParcelType>
template<class TrackData>
void Foam::ThermoParcel<ParcelType>::setCellValues
(
    TrackData& td,
    const scalar dt,
    const label celli
)
{
    ParcelType::setCellValues(td, dt, celli);

    tetIndices tetIs = this->currentTetIndices();

    Cpc_ = td.CpInterp().interpolate(this->coordinates(), tetIs);

    Tc_ = td.TInterp().interpolate(this->coordinates(), tetIs);

    if (Tc_ < td.cloud().constProps().TMin())
    {
        if (debug)
        {
            WarningInFunction
                << "Limiting observed temperature in cell " << celli << " to "
                << td.cloud().constProps().TMin() <<  nl << endl;
        }

        Tc_ = td.cloud().constProps().TMin();
    }
}


template<class ParcelType>
template<class TrackData>
void Foam::ThermoParcel<ParcelType>::cellValueSourceCorrection
(
    TrackData& td,
    const scalar dt,
    const label celli
)
{
    this->Uc_ += td.cloud().UTrans()[celli]/this->massCell(celli);

    const scalar CpMean = td.CpInterp().psi()[celli];
    Tc_ += td.cloud().hsTrans()[celli]/(CpMean*this->massCell(celli));

    if (Tc_ < td.cloud().constProps().TMin())
    {
        if (debug)
        {
            WarningInFunction
                << "Limiting observed temperature in cell " << celli << " to "
                << td.cloud().constProps().TMin() <<  nl << endl;
        }

        Tc_ = td.cloud().constProps().TMin();
    }
}


template<class ParcelType>
template<class TrackData>
void Foam::ThermoParcel<ParcelType>::calcSurfaceValues
(
    TrackData& td,
    const label celli,
    const scalar T,
    scalar& Ts,
    scalar& rhos,
    scalar& mus,
    scalar& Pr,
    scalar& kappas
) const
{
    // Surface temperature using two thirds rule
    Ts = (2.0*T + Tc_)/3.0;

    if (Ts < td.cloud().constProps().TMin())
    {
        if (debug)
        {
            WarningInFunction
                << "Limiting parcel surface temperature to "
                << td.cloud().constProps().TMin() <<  nl << endl;
        }

        Ts = td.cloud().constProps().TMin();
    }

    // Assuming thermo props vary linearly with T for small d(T)
    const scalar TRatio = Tc_/Ts;

    rhos = this->rhoc_*TRatio;

    tetIndices tetIs = this->currentTetIndices();
    mus = td.muInterp().interpolate(this->coordinates(), tetIs)/TRatio;
    kappas = td.kappaInterp().interpolate(this->coordinates(), tetIs)/TRatio;

    Pr = Cpc_*mus/kappas;
    Pr = max(ROOTVSMALL, Pr);
}


template<class ParcelType>
template<class TrackData>
void Foam::ThermoParcel<ParcelType>::calc
(
    TrackData& td,
    const scalar dt,
    const label celli
)
{
    // Define local properties at beginning of time step
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    const scalar np0 = this->nParticle_;
    const scalar mass0 = this->mass();

    // Store T for consistent radiation source
    const scalar T0 = this->T_;


    // Calc surface values
    // ~~~~~~~~~~~~~~~~~~~
    scalar Ts, rhos, mus, Pr, kappas;
    calcSurfaceValues(td, celli, this->T_, Ts, rhos, mus, Pr, kappas);

    // Reynolds number
    scalar Re = this->Re(this->U_, this->d_, rhos, mus);


    // Sources
    // ~~~~~~~

    // Explicit momentum source for particle
    vector Su = Zero;

    // Linearised momentum source coefficient
    scalar Spu = 0.0;

    // Momentum transfer from the particle to the carrier phase
    vector dUTrans = Zero;

    // Explicit enthalpy source for particle
    scalar Sh = 0.0;

    // Linearised enthalpy source coefficient
    scalar Sph = 0.0;

    // Sensible enthalpy transfer from the particle to the carrier phase
    scalar dhsTrans = 0.0;


    // Heat transfer
    // ~~~~~~~~~~~~~

    // Sum Ni*Cpi*Wi of emission species
    scalar NCpW = 0.0;

    // Calculate new particle temperature
    this->T_ =
        this->calcHeatTransfer
        (
            td,
            dt,
            celli,
            Re,
            Pr,
            kappas,
            NCpW,
            Sh,
            dhsTrans,
            Sph
        );


    // Motion
    // ~~~~~~

    // Calculate new particle velocity
    this->U_ =
        this->calcVelocity(td, dt, celli, Re, mus, mass0, Su, dUTrans, Spu);


    //  Accumulate carrier phase source terms
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    if (td.cloud().solution().coupled())
    {
        // Update momentum transfer
        td.cloud().UTrans()[celli] += np0*dUTrans;

        // Update momentum transfer coefficient
        td.cloud().UCoeff()[celli] += np0*Spu;

        // Update sensible enthalpy transfer
        td.cloud().hsTrans()[celli] += np0*dhsTrans;

        // Update sensible enthalpy coefficient
        td.cloud().hsCoeff()[celli] += np0*Sph;

        // Update radiation fields
        if (td.cloud().radiation())
        {
            // ankur
            //const scalar ap = this->areaP();
            //const scalar T4 = pow4(T0);
            //const label nBands=td.cloud().nBands();
            //td.cloud().updateRadProperties(celli,ap,np0,T4,dt);

//            for(label bandI=0; bandI < nBands; bandI++)
//            {
//                td.cloud().radAreaP(bandI)[celli] += dt*np0*ap;
//                td.cloud().radT4(bandI)[celli] += dt*np0*T4;
//                td.cloud().radAreaPT4(bandI)[celli] += dt*np0*ap*T4;
//            }

//            td.cloud().radAreaP()[celli] += dt*np0*ap;
//            td.cloud().radT4()[celli] += dt*np0*T4;
//            td.cloud().radAreaPT4()[celli] += dt*np0*ap*T4;
        }
    }
}


template<class ParcelType>
template<class TrackData>
Foam::scalar Foam::ThermoParcel<ParcelType>::calcHeatTransfer
(
    TrackData& td,
    const scalar dt,
    const label celli,
    const scalar Re,
    const scalar Pr,
    const scalar kappa,
    const scalar NCpW,
    const scalar Sh,
    scalar& dhsTrans,
    scalar& Sph
)
{
    if (!td.cloud().heatTransfer().active())
    {
        return T_;
    }

    const scalar d = this->d();
    const scalar rho = this->rho();

    // Calc heat transfer coefficient
    scalar htc = td.cloud().heatTransfer().htc(d, Re, Pr, kappa, NCpW);

    if (mag(htc) < ROOTVSMALL && !td.cloud().radiation())
    {
        return
            max
            (
                T_ + dt*Sh/(this->volume(d)*rho*Cp_),
                td.cloud().constProps().TMin()
            );
    }

    htc = max(htc, ROOTVSMALL);
    const scalar As = this->areaS(d);

    scalar ap = Tc_ + Sh/(As*htc);
    scalar bp = 6.0*(Sh/As + htc*(Tc_ - T_));
    if (td.cloud().radiation())
    {
        tetIndices tetIs = this->currentTetIndices();

        //- The call below updates radiation fields and computes parcel specific terms for radiative src calculations 
        List<scalar> kgAndkEmm(td.cloud().radCalc(celli,this->areaP(),this->nParticle_,pow4(T_),dt)); // ankur

        if (td.cloud().coupledRadiation()) // ankur
        {
            // const scalar Gc = td.GInterp().interpolate(this->position(), tetIs);
            const scalar kG = kgAndkEmm[0]; // td.cloud().getkG(d);    // multiple of k*G   // unit of k here is dimensionless // ankur
            const scalar kEmm = kgAndkEmm[1]; // td.cloud().getEmm(d);  //   emissivity of the particle // dimensionless number // ankur
            const scalar sigma = physicoChemical::sigma.value(); // ankur
            // const scalar epsilon = td.cloud().constProps().epsilon0();

            // Assume constant source
            // scalar s = epsilon*(Gc/4.0 - sigma*pow4(T_));
            scalar s = (kG/4.) - sigma*kEmm*pow4(T_);  // ankur

            ap += s/htc;
            bp += 6.0*s;
        }
    }
    bp /= rho*d*Cp_*(ap - T_) + ROOTVSMALL;

    // Integrate to find the new parcel temperature
    IntegrationScheme<scalar>::integrationResult Tres =
        td.cloud().TIntegrator().integrate(T_, dt, ap*bp, bp);

    scalar Tnew =
        min
        (
            max
            (
                Tres.value(),
                td.cloud().constProps().TMin()
            ),
            td.cloud().constProps().TMax()
        );

    Sph = dt*htc*As;

    dhsTrans += Sph*(Tres.average() - Tc_);

    return Tnew;
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

template<class ParcelType>
Foam::ThermoParcel<ParcelType>::ThermoParcel
(
    const ThermoParcel<ParcelType>& p
)
:
    ParcelType(p),
    T_(p.T_),
    Cp_(p.Cp_),
    Tc_(p.Tc_),
    Cpc_(p.Cpc_)
{}


template<class ParcelType>
Foam::ThermoParcel<ParcelType>::ThermoParcel
(
    const ThermoParcel<ParcelType>& p,
    const polyMesh& mesh
)
:
    ParcelType(p, mesh),
    T_(p.T_),
    Cp_(p.Cp_),
    Tc_(p.Tc_),
    Cpc_(p.Cpc_)
{}


// * * * * * * * * * * * * * * IOStream operators  * * * * * * * * * * * * * //

#include "ThermoParcelIO.C"

// ************************************************************************* //
