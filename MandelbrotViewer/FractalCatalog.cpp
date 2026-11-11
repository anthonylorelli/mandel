#include "pch.h"
#include "FractalCatalog.h"

namespace Mandelbrot
{
    std::wstring GetFractalTypeName(FractalType type)
    {
        switch (type)
        {
        case FractalType::Mandelbrot:     return L"Mandelbrot Set";
        case FractalType::BurningShip:    return L"Burning Ship";
        case FractalType::Tricorn:        return L"Tricorn (Mandelbar)";
        case FractalType::Celtic:         return L"Celtic Mandelbrot";
        case FractalType::Buffalo:        return L"Buffalo Fractal";
        case FractalType::CubicMultibrot: return L"Cubic Multibrot";
        case FractalType::Phoenix:        return L"Phoenix Fractal";
        case FractalType::Newton:         return L"Newton's Fractal";
        default:                          return L"Unknown";
        }
    }

    const std::vector<FractalType>& GetAllFractalTypes()
    {
        static const std::vector<FractalType> types = {
            FractalType::Mandelbrot,
            FractalType::BurningShip,
            FractalType::Tricorn,
            FractalType::Celtic,
            FractalType::Buffalo,
            FractalType::CubicMultibrot,
            FractalType::Phoenix,
            FractalType::Newton
        };
        return types;
    }

    const std::vector<BookmarkLocation>& GetBookmarks(FractalType type)
    {
        static const std::vector<BookmarkLocation> mandelbrotBookmarks = {
            { L"Overview", L"Full view of the iconic Mandelbrot set", -0.75, 0.0, 1.0, 300 },
            { L"Seahorse Valley", L"Intricate spiral filaments connecting cardioid and bulb", -0.7436438870371587, 0.1318259042053119, 85000.0, 1000 },
            { L"Elephant Valley", L"Elephant trunk formations on the East side of main cardioid", 0.274, 0.482, 250.0, 500 },
            { L"Triple Spiral", L"Hypnotic three-arm vortex of fractal energy", -0.088886, 0.654805, 1600.0, 800 },
            { L"Mini Mandelbrot Satellite", L"Self-similar satellite island along the primary spike", -1.768778833, -0.001738994, 30000.0, 1500 },
            { L"Scepter Valley", L"Branching candelabras and crowned antennae", -1.25066, 0.02012, 1000.0, 600 },
            { L"Deep Spiral Core", L"Deep zoom revealing dense cosmic spiral clusters", -0.7436438870371587, 0.1318259042053119, 500000.0, 2000 }
        };

        static const std::vector<BookmarkLocation> burningShipBookmarks = {
            { L"Burning Ship Overview", L"Full view of the burning ship with glowing masts", -0.45, 0.5, 1.2, 400 },
            { L"The Grand Mast", L"Towering mast and rigging formations rising into the sky", -1.75, 0.03, 35.0, 800 },
            { L"The Armada Fleet", L"Endless miniature burning ships aligned along the horizon", -1.762, 0.028, 250.0, 1200 },
            { L"Celtic Cross", L"Intricate cross-shaped filigree deep within the ship framework", -1.861, 0.0, 800.0, 1500 },
            { L"Central Mast & Rigging", L"Towering spire and intricate ember rigging", -0.48, 0.86, 20.0, 600 },
            { L"Keel & Burning Ribs", L"Structural skeletal beam embers along the bottom keel", -0.32, -0.15, 15.0, 500 },
            { L"Ghost Ship Satellite", L"Self-similar phantom ship adrift in deep fractal space", -1.7495, 0.0255, 1800.0, 2000 }
        };

        static const std::vector<BookmarkLocation> tricornBookmarks = {
            { L"Tricorn Overview", L"Iconic three-cornered cardioid boundary and antennas", -0.4, 0.0, 1.0, 300 },
            { L"Triple Corner Cusp", L"Sharp triangular pinch point connecting perimeter ridges", 0.25, 0.0, 15.0, 500 },
            { L"Cauliflower Valley", L"Curving cauliflower plumes with anti-holomorphic symmetry", 0.0, 0.30, 25.0, 800 },
            { L"Filament Valley Web", L"Intricate filament network connecting outer cusps", 0.20, 0.50, 30.0, 800 },
            { L"Upper Horn Antenna", L"Towering crystal antenna horn reaching into outer space", 0.45, 1.00, 20.0, 600 },
            { L"Mini Tricorn Satellite", L"Perfect miniature replica of the main tricorn body", -1.40, 0.0, 100.0, 1000 },
            { L"Deep Tricorn Core", L"Dense geometric crystalline lattice in deep zoom", -1.4011, 0.0, 3000.0, 1500 }
        };

        static const std::vector<BookmarkLocation> newtonBookmarks = {
            { L"Overview", L"Full 3-fold symmetric view of the three attraction basins", 0.0, 0.0, 1.0, 100 },
            { L"Origin Rosette", L"Intricate self-similar rosettes where all three basins meet", 0.0, 0.0, 8.0, 150 },
            { L"Deep Origin Core", L"Infinite recursive rosette petals meeting at the singularity", 0.0, 0.0, 60.0, 250 },
            { L"Triple Basin Junction", L"Pinched boundary filaments where all three colors converge", -0.75, 0.0, 15.0, 200 },
            { L"Basin Boundary Web", L"Interlocking fractal webs along the domain threshold", -0.60, 0.20, 15.0, 200 },
            { L"Outer Pinched Filament", L"Needle-thin pinched filigree between alternating lobes", -1.40, 0.0, 15.0, 200 },
            { L"Interlocking Lobe Vortex", L"Swirling vortex of interleaved color basins", -1.20, 0.20, 15.0, 200 }
        };

        static const std::vector<BookmarkLocation> celticBookmarks = {
            { L"Celtic Overview", L"Full view of the folded Celtic knotwork shield", -0.35, 0.0, 1.0, 300 },
            { L"Braided Ribbon", L"Interwoven knotwork ribbons along the upper fold", -0.75, 0.12, 40.0, 600 },
            { L"Western Shield Boss", L"Symmetrical spiked medallion on the western spearhead", -1.75, 0.0, 80.0, 800 },
            { L"Crown Filigree", L"Ornate braided arches along the northern ridge", -0.76264658, 0.55803571, 35.0, 700 },
            { L"Knotwork Spiral", L"Coiled spirals interlocking with folded creases", -1.2706251, 0.16593745, 60.0, 800 },
            { L"Eastern Cusp", L"Sharp folded cusp where real-axis reflection meets", -0.10091822, -0.36870117, 25.0, 600 },
            { L"Deep Celtic Medallion", L"Nested self-similar shield island deep in the spear", -0.75528991, 0.51689995, 400.0, 1200 }
        };

        static const std::vector<BookmarkLocation> buffaloBookmarks = {
            { L"Buffalo Overview", L"Full silhouette of the horned Buffalo fractal", -0.45, 0.35, 1.0, 300 },
            { L"Western Spine", L"Serrated dorsal ridge and miniature buffalo satellites", -1.76, 0.02, 60.0, 800 },
            { L"Horn Tip Arch", L"Sweeping cathedral arches along the upper horn", 0.08179198, 1.0611132, 25.0, 600 },
            { L"Forehead Filigree", L"Symmetrical folded lattice above the eastern cusp", 0.20870367, 0.90309527, 25.0, 600 },
            { L"Lower Rib Web", L"Delicate ember webbing beneath the main body", -0.4743323, -0.32101934, 30.0, 700 },
            { L"Twin Horn Valley", L"Deep cleft separating the primary and secondary horns", -1.4469982, 0.85565674, 45.0, 800 },
            { L"Phantom Buffalo", L"Self-similar miniature buffalo embedded in the western needle", -1.5047967, 0.00089792, 400.0, 1200 }
        };

        static const std::vector<BookmarkLocation> cubicMultibrotBookmarks = {
            { L"Cubic Overview", L"Twin-lobed symmetry of the degree-3 Multibrot set", 0.0, 0.0, 1.0, 300 },
            { L"Northern Star", L"Four-fold star satellite crowning the upper antenna", -0.29520097, 0.68994895, 45.0, 800 },
            { L"East Cusp Valley", L"Double seahorse spirals converging at the eastern pinch", 0.59173641, 0.47172444, 35.0, 700 },
            { L"West Cusp Valley", L"Mirrored spiral canyon on the western cardioid pinch", -0.60583013, 0.48777918, 35.0, 700 },
            { L"Upper Lobe Spiral", L"Hypnotic galaxy arms along the upper-right shoulder", 0.31423136, 0.6763531, 45.0, 800 },
            { L"Southern Crown", L"Symmetric crystalline antenna at the southern pole", 0.31114628, -0.68133564, 50.0, 800 },
            { L"Deep Cubic Satellite", L"Miniature twin-lobed Multibrot island on the northern spike", -0.15849872, 0.74878064, 300.0, 1200 }
        };

        static const std::vector<BookmarkLocation> phoenixBookmarks = {
            { L"Phoenix Overview", L"Outspread feathered wings of Ushiki's Phoenix fractal", 0.0, 0.0, 1.0, 300 },
            { L"Wingtip Plume", L"Layered feather barbs sweeping across the outer wing", -0.45, 0.25, 15.0, 500 },
            { L"Feather Spiral", L"Coiled plumage vortex inside the right wing", 0.32, 0.18, 40.0, 800 },
            { L"Phoenix Crest", L"Ornate crown feathers rising from the central body", 0.56697573, 0.11594143, 25.0, 600 },
            { L"Tail Plume Filigree", L"Symmetrical tail feathers trailing below the core", -0.33037393, 0.07242102, 25.0, 600 },
            { L"Inner Wing Vortex", L"Interlocking spiral quills along the primary wing joint", -0.46998201, -0.1845639, 40.0, 800 },
            { L"Deep Plumage Rosette", L"Intricate self-similar feather rosette at high magnification", -0.31079524, 0.28984978, 250.0, 1200 }
        };

        switch (type)
        {
        case FractalType::BurningShip:
            return burningShipBookmarks;
        case FractalType::Tricorn:
            return tricornBookmarks;
        case FractalType::Celtic:
            return celticBookmarks;
        case FractalType::Buffalo:
            return buffaloBookmarks;
        case FractalType::CubicMultibrot:
            return cubicMultibrotBookmarks;
        case FractalType::Phoenix:
            return phoenixBookmarks;
        case FractalType::Newton:
            return newtonBookmarks;
        case FractalType::Mandelbrot:
        default:
            return mandelbrotBookmarks;
        }
    }
}
