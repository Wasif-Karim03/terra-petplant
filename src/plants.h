// plants.h — curated houseplant care database (on-device, offline).
// Picking a plant tunes Aiko's thirst/temperature thresholds and shows a care
// guide on the settings page. soilDry/soilWet = moisture % thresholds;
// tMin/tMax = comfortable temperature range in Celsius.
#pragma once
#include <stdint.h>

struct Plant {
  const char *key; const char *name;
  uint8_t soilDry, soilWet;  // below soilDry% = thirsty; above soilWet% = overwatered
  int8_t  tMin, tMax;        // Celsius comfort range
  const char *light;         // light needs
  const char *water;         // watering guidance
  const char *care;          // one extra tip
};

static const Plant PLANTS[] = {
 {"pothos","Pothos",25,80,12,30,"Low to bright indirect","Water when the top inch of soil is dry (about weekly).","Very forgiving - a perfect starter plant."},
 {"snake","Snake Plant",15,60,12,32,"Low to bright indirect","Let it dry out fully; water every 2-3 weeks.","Nearly indestructible; hates soggy soil."},
 {"monstera","Monstera",30,80,15,30,"Bright indirect","Water when the top 2 inches are dry (about weekly).","Loves humidity; wipe the big leaves."},
 {"peacelily","Peace Lily",40,85,15,30,"Low to medium indirect","Keep evenly moist; it droops when thirsty.","It literally tells you when it needs water."},
 {"spider","Spider Plant",30,80,12,30,"Bright indirect","Water when the top is dry (about weekly).","Easy; likes to dry a little between waterings."},
 {"zz","ZZ Plant",15,60,15,30,"Low to bright indirect","Water every 2-3 weeks; very drought tolerant.","Thrives on neglect - do not overwater."},
 {"succulent","Succulent",12,55,10,35,"Bright, some direct sun","Soak, then let it dry out fully (about 2 weeks).","Never let it sit in water."},
 {"cactus","Cactus",10,50,10,40,"Full sun","Very little water; every 2-4 weeks.","Minimal water, maximum light."},
 {"aloe","Aloe Vera",15,55,12,32,"Bright indirect to some sun","Deep water only when fully dry.","A succulent - good drainage is essential."},
 {"fiddle","Fiddle Leaf Fig",30,78,15,30,"Bright indirect","Water when the top 2 inches are dry.","Fussy; dislikes being moved or drafts."},
 {"rubber","Rubber Plant",30,78,15,30,"Bright indirect","Water when the top of the soil is dry.","Wipe leaves; likes steady conditions."},
 {"philodendron","Philodendron",28,80,15,30,"Low to bright indirect","Water when the top inch is dry.","Easy, fast grower."},
 {"fern","Boston Fern",45,88,15,28,"Medium indirect","Keep consistently moist; loves humidity.","Mist often; hates drying out."},
 {"ivy","English Ivy",30,80,10,28,"Bright indirect","Water when the top is dry.","Prefers cooler spots."},
 {"jade","Jade Plant",15,55,12,30,"Bright, some sun","Water only when fully dry.","A succulent; water sparingly."},
 {"basil","Basil",40,85,15,32,"Full sun / very bright","Keep moist; water when the top is dry.","Pinch the tops; it loves light."},
 {"mint","Mint",45,88,12,30,"Bright indirect to some sun","Keep the soil moist.","Vigorous grower; likes water."},
 {"orchid","Orchid",25,70,15,30,"Bright indirect","Water about weekly; let roots dry between.","Needs special bark mix; do not overwater."},
 {"anthurium","Anthurium",35,82,16,30,"Bright indirect","Keep lightly moist.","Loves humidity; long-lasting blooms."},
 {"calathea","Calathea",40,85,16,28,"Medium indirect","Keep moist with filtered/rain water.","A humidity lover and a bit dramatic."},
 {"dracaena","Dracaena",25,75,15,30,"Low to bright indirect","Water when the top is dry.","Sensitive to fluoride in tap water."},
 {"aglaonema","Chinese Evergreen",30,80,15,30,"Low to medium","Water when the top is dry.","One of the best low-light plants."},
 {"bamboo","Lucky Bamboo",50,95,15,30,"Low to bright indirect","Keep roots in water or moist soil.","Often grown in just water."},
 {"violet","African Violet",35,80,16,27,"Bright indirect","Water from the bottom; keep lightly moist.","Avoid getting the leaves wet."},
 {"begonia","Begonia",35,82,15,28,"Bright indirect","Keep lightly moist - do not soak.","Great foliage and flowers."},
 {"croton","Croton",35,82,16,30,"Bright, some sun","Keep moist; loves humidity.","Bright colors need bright light."},
 {"lavender","Lavender",15,55,10,32,"Full sun","Water when dry; needs great drainage.","Hates wet feet; loves sunshine."},
 {"rosemary","Rosemary",20,60,10,32,"Full sun","Water when the top is dry.","An herb - it needs lots of light."},
 {"moneytree","Money Tree",28,78,15,30,"Bright indirect","Water when the top 2 inches are dry.","Do not overwater; likes routine."},
 {"birdofparadise","Bird of Paradise",30,80,15,30,"Bright, some direct sun","Water when the top is dry.","Wants lots of light to thrive."},
};
static const int PLANTS_N = sizeof(PLANTS) / sizeof(PLANTS[0]);
