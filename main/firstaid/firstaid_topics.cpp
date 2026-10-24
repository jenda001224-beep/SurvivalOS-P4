/*
 * @Description: First Aid guide: the topics (Czech)
 * @License: GPL 3.0
 *
 * Sources: European Resuscitation Council Guidelines 2021 (basic life support,
 * first aid), IFRC International First Aid, Resuscitation and Education
 * Guidelines 2020, Czech Red Cross first aid manual, WHO guidance on oral
 * rehydration and on snakebite, wilderness first aid practice (WMS 2019).
 *
 * The phone is meant for when no rescue service can be called, so the guides
 * never say "call 155": they list the danger signs, what to do alone, and when
 * to get the person to medical care as soon as it can be done.
 */
#include <cstring>

#include "firstaid/firstaid_content.h"

namespace lilygo_box::firstaid {
namespace {

using C = Category;
using U = Urgency;

constexpr Topic kTopics[] = {
// ── Život ohrožující stavy ──────────────────────────────────────────────────
{"unconscious", "Bezvědomí", C::kEmergency, U::kEmergency,
 "bezvedomi bezvědomí nereaguje omdlel omdlela lezi nehybne zotavovaci poloha stabilizovana",
 "Vždy, když člověk nereaguje na oslovení ani zatřesení.",
 "Zajisti své bezpečí (provoz, elektřina, plyn, oheň).\n"
 "Hlasitě oslov a zatřes rameny. Nereaguje? Zavolej na lidi kolem.\n"
 "Polož ho na záda, zakloň hlavu (ruka na čelo, dva prsty pod bradu).\n"
 "Dívej se, poslouchej a cíť dech až 10 sekund.\n"
 "NEDÝCHÁ nebo dýchá jen lapavě: hned začni resuscitaci, viz Resuscitace.\n"
 "DÝCHÁ normálně: otoč ho do zotavovací polohy na bok, hlavu zakloněnou, ústa směrem k zemi.\n"
 "Každou minutu kontroluj dech. Nenechávej ho samotného; jakmile to jde, zajisti převoz k lékaři.\n"
 "Přikryj ho, aby neprochladl.",
 "Nedávej nic do úst, ani pití ani léky.\n"
 "Nenech ho ležet na zádech bez dozoru – může se udusit zvratky.\n"
 "Nepodkládej hlavu polštářem.",
 "", "", "cpr choking stroke seizure"},

{"cpr", "Resuscitace (KPR) – dospělý", C::kEmergency, U::kEmergency,
 "resuscitace kpr masaz srdce nedycha zastava srdce masáž oživování defibrilator aed",
 "Kdo nereaguje a nedýchá normálně, má zástavu oběhu. Bez resuscitace zemře během minut.",
  "Je-li vás víc, ať někdo hledá AED (defibrilátor) – bývá na nádražích, v obchodech, úřadech.\n"
 "Klekni si vedle, dlaně na sebe doprostřed hrudníku (dolní polovina hrudní kosti).\n"
 "Natažené paže, tlač kolmo dolů 5–6 cm hluboko, 100–120× za minutu (rytmus písně Stayin' Alive).\n"
 "Po každém stlačení nech hrudník úplně povolit.\n"
 "Umíš a chceš dýchat: po 30 stlačeních 2 vdechy (zaklonit hlavu, ucpat nos, vdech 1 s, hrudník se zvedne). Jinak jen nepřetržitě stlačuj.\n"
 "Přinesou AED: zapni ho a dělej, co říká. Elektrody na holý hrudník podle obrázku.\n"
 "Nepřestávej, dokud se člověk neprobere a nezačne normálně dýchat, nebo už nemůžeš. Ve dvou se střídejte každé 2 minuty.\n"
 "Daleko od pomoci lze po 30 minutách bez známek života přestat. Výjimka: podchlazení, tonutí, zásah bleskem – tam pokračuj déle.",
 "Neztrácej čas hledáním pulsu.\n"
 "Nepřerušuj stlačování na déle než 10 sekund.\n"
 "Neboj se zlomených žeber – bez masáže člověk zemře.",
 "", "", "cpr_child unconscious drowning electric"},

{"cpr_child", "Resuscitace – dítě a kojenec", C::kEmergency, U::kEmergency,
 "resuscitace dite kojenec miminko nedycha batole kpr deti",
 "Dítě nereaguje a nedýchá normálně.",
 "Zakloň hlavu (kojenec jen do neutrální polohy) a zkontroluj dech 10 s.\n"
 "Začni 5 vdechy: kojenci ústy přes ústa i nos, dítěti ústy do úst s ucpaným nosem.\n"
 "Pak 30 stlačení : 2 vdechy, rychlost 100–120 za minutu.\n"
 "Kojenec (do 1 roku): stlačuj dvěma prsty nebo dvěma palci na dolní třetině hrudní kosti, asi 4 cm.\n"
 "Dítě: dlaní jedné ruky (u většího dítěte obě ruce) do středu hrudníku, asi 5 cm, tedy třetinu hloubky hrudníku.\n"
 "AED lze použít i u dětí – u malých dětí dětské elektrody, jinak dospělé (jedna vpředu, druhá na zádech).\n"
 "Pokračuj, dokud dítě nezačne dýchat nebo už nemůžeš.",
 "Netřes dítětem.\n"
 "Nepřestávej kvůli hledání pulsu.",
 "", "", "cpr choking_child"},

{"choking", "Dušení – dospělý", C::kEmergency, U::kEmergency,
 "duseni dušení zaskocilo zaskočilo nemuze dychat sousto v krku kasle cizí těleso heimlich",
 "Když přestane účinně kašlat, nemůže mluvit nebo dýchat, modrá, nebo upadne do bezvědomí.",
 "Kašle-li účinně, povzbuzuj ho ke kašli a nic jiného nedělej.\n"
 "Přestává kašlat nebo nemůže dýchat: předkloň ho a dej až 5 ran mezi lopatky hranou dlaně.\n"
 "Nepomohlo: až 5 stlačení nadbřišku (Heimlichův manévr) – stoupni si za něj, pěst mezi pupek a hrudní kost, druhou rukou chyť a prudce trhni dovnitř a nahoru.\n"
 "Střídej 5 úderů do zad a 5 stlačení, dokud těleso nevyletí.\n"
  "Upadne do bezvědomí: polož ho na zem a začni resuscitaci (stlačování hrudníku může těleso uvolnit).\n"
 "Po Heimlichově manévru musí vždy k lékaři – může mít vnitřní poranění.",
 "Nesahej naslepo prsty do krku.\n"
 "Těhotným a velmi obézním místo nadbřišku stlačuj hrudník.",
 "", "", "choking_child cpr"},

{"choking_child", "Dušení – dítě a kojenec", C::kEmergency, U::kEmergency,
 "duseni dite kojenec miminko zaskocilo hracka v krku nemuze dychat",
 "Když dítě nemůže kašlat, dýchat, modrá nebo ztrácí vědomí.",
 "Dítě účinně kašle: povzbuzuj ho, nic nedělej.\n"
 "KOJENEC: polož si ho na předloktí hlavou dolů, podpírej čelist, a dej 5 úderů do zad mezi lopatky.\n"
 "Pak ho otoč na záda, hlavou níž, a dvěma prsty dej 5 stlačení hrudníku (jako při resuscitaci, ale pomaleji a prudčeji).\n"
 "DÍTĚ nad 1 rok: předkloň ho, 5 úderů mezi lopatky, pak 5 stlačení nadbřišku jako u dospělého.\n"
 "Střídej, dokud těleso nevypadne.\n"
 "Bezvědomí: 5 vdechů a resuscitace 30:2, před každým vdechem se podívej do úst – viditelné těleso vyndej.",
 "U kojence nikdy nestlačuj břicho.\n"
 "Nedrž dítě za nohy hlavou dolů.\n"
 "Nesahej naslepo do krku.",
 "", "", "cpr_child choking"},

{"bleeding", "Silné krvácení", C::kEmergency, U::kEmergency,
 "krvaceni krvácení silne krev teče stříká tepna masivni turniket skrtidlo tlakovy obvaz",
 "Vždy u silného krvácení. Krev stříká nebo rychle teče, nejde zastavit tlakem, nebo je člověk bledý, studený a zmatený.",
 "Natáhni rukavice nebo sáček na ruku, je-li po ruce – ale nezdržuj se.\n"
 "Přímo na ránu silně tlač dlaní přes čistou látku, obvaz nebo oblečení.\n"
 "Člověka posaď nebo polož.\n"
 "Tlač nepřetržitě aspoň 10 minut, neodkrývej ránu.\n"
 "Prosakuje: přidej další vrstvu a tlač dál, nebo přilož tlakový obvaz (viz Tlakový obvaz).\n"
 "Život ohrožující krvácení z končetiny, které tlak nezastaví: škrtidlo (turniket) 5–7 cm nad ránu, utahuj, až krvácení přestane. Zapiš čas utažení. Nepovoluj.\n"
 "Hluboká rána na trupu nebo krku: ucpi ji (vtlač do ní obvaz nebo látku) a tlač.\n"
 "Pak hlídej šok: lehnout, přikrýt, nic nejíst, nepít.",
 "Nepovoluj tlak, abys kontroloval.\n"
 "Nevyndávej z rány zabodnuté předměty – obvaž kolem nich.\n"
 "Nepoužívej škrtidlo na krk, trup, ani u menšího krvácení.",
 "", "", "bandage_pressure shock amputation wound_cut"},

{"shock", "Šok", C::kEmergency, U::kEmergency,
 "sok šok bledy studeny pot zrychleny tep slabost po urazu ztrata krve",
 "Bledý, studený, opocený, zrychlený slabý tep, zmatenost – šok ohrožuje život. Zastav příčinu a co nejdřív dostaň člověka k lékařské péči.",
 "Odstraň příčinu, je-li to možné (zastav krvácení, chlaď popáleninu).\n"
 "Polož ho na záda. Při bezvědomí do zotavovací polohy.\n"
 "Uvolni těsný oděv, přikryj ho proti chladu, i zespodu.\n"
 "Uklidňuj, mluv s ním, nenechávej ho samotného.\n"
 "Sleduj dech a vědomí.",
 "Nedávej jíst. Pít jen po malých doušcích, je-li při plném vědomí a pomoc je daleko.\n"
 "Nezvedej nohy, jsou-li zraněné nebo je podezření na zlomeninu pánve či páteře.",
 "", "", "bleeding burns anaphylaxis"},

{"heart_attack", "Bolest na hrudi, infarkt", C::kEmergency, U::kEmergency,
 "infarkt bolest na hrudi tlak na hrudi srdce svira hrudnik vystreluje do ruky celist",
 "Tlak nebo pálení na hrudi déle než pár minut, vystřeluje do paže, čelisti, zad, s pocením, dušností nebo nevolností.",
 "Úplný klid, žádná námaha – co nejdřív k lékařské péči (nést nebo vézt, ne chodit).\n"
 "Posaď ho do pohodlné polohy (polosed), uvolni oděv, uklidňuj.\n"
 "Není-li alergický na aspirin, nekrvácí a nebere léky na ředění krve: ať rozkouše 1 tabletu kyseliny acetylsalicylové (250–500 mg, Aspirin, Acylpyrin).\n"
 "Má-li vlastní nitroglycerin od lékaře, ať ho použije podle návodu.\n"
 "Upadne do bezvědomí a nedýchá: resuscitace.",
 "Nenech ho chodit ani se namáhat.\n"
 "Nepodávej aspirin dětem.",
 "acetylsalicylic", "", "cpr stroke"},

{"stroke", "Mrtvice (cévní mozková příhoda)", C::kEmergency, U::kEmergency,
 "mrtvice mozkova prihoda pokleslý koutek povisly ochrnuti mluvi nesrozumitelne slabost ruky fast",
 "Kterýkoli z příznaků FAST. Léčba v nemocnici pomáhá jen v prvních hodinách – každá minuta znamená miliony mozkových buněk.",
 "Test FAST: Face – pokleslý koutek úst při úsměvu. Arm – nezvedne obě paže stejně. Speech – mluví nesrozumitelně. Time – čas: co nejrychleji do nemocnice.\n"
 "Zapiš čas, kdy příznaky začaly nebo kdy byl naposledy v pořádku – lékaři to potřebují.\n"
 "Posaď nebo polož se zvednutou hlavou, uklidňuj.\n"
 "Bezvědomí s dýcháním: zotavovací poloha na ochrnutý bok.",
 "Nedávej jíst, pít ani léky (ani aspirin – může jít o krvácení do mozku).",
 "", "", "unconscious"},

{"seizure", "Epileptický záchvat, křeče", C::kEmergency, U::kDoctor,
 "kreče křeče epilepsie zachvat zachvat cuka padá pěna u úst",
 "Záchvat trvá déle než 5 minut, opakuje se, je první v životě, člověk se zranil, je ve vodě, těhotná, nebo se po záchvatu neprobírá.",
 "Zachovej klid a změř čas.\n"
 "Odstraň z okolí ostré a tvrdé předměty, podlož hlavu (bunda, svetr).\n"
 "Po skončení křečí ho otoč do zotavovací polohy a kontroluj dech.\n"
 "Zůstaň u něj, dokud se úplně neprobere – bývá zmatený a ospalý.",
 "Nedrž ho, nebraň křečím.\n"
 "Nic mu nedávej do úst, nepáčí zuby.\n"
 "Nedávej pít, dokud není plně při vědomí.",
 "", "", "unconscious febrile_seizure"},

{"febrile_seizure", "Horečnaté křeče u dítěte", C::kEmergency, U::kDoctor,
 "horecnate krece dite horecka kreče batole",
 "První křeče v životě, trvají déle než 5 minut, opakují se, dítě je mladší 6 měsíců nebo se neprobouzí.",
 "Polož dítě na bok na bezpečné místo, odstraň předměty kolem.\n"
 "Svleč ho do lehkého oblečení.\n"
 "Po záchvatu ho nech na boku a sleduj dech.\n"
 "Až je plně při vědomí, lze podat lék na horečku podle váhy (paracetamol nebo ibuprofen).",
 "Nedávej nic do úst během křečí.\n"
 "Nechlaď ledovou vodou ani alkoholem.",
 "paracetamol ibuprofen", "", "fever seizure"},

{"asthma", "Astmatický záchvat, dušnost", C::kEmergency, U::kDoctor,
 "astma dusnost dušnost piskot pri dychani nemuze vydechnout inhalator",
 "Inhalátor nepomáhá, nemůže mluvit celé věty, modrá, je vyčerpaný nebo zmatený.",
 "Posaď ho, ať se předkloní a opře o ruce (vsedě nebo vestoje u stolu).\n"
 "Ať použije svůj úlevový inhalátor (modrý, salbutamol): 1 vdech, nejlépe přes nástavec, opakovat až 10 vdechů (1 každé 30–60 s).\n"
 "Uklidňuj, ať dýchá pomalu.\n"
 "Bez zlepšení za 10 minut opakuj inhalátor; těžký záchvat je důvod co nejdřív k lékaři.",
 "Neukládej ho do lehu.",
 "", "", "anaphylaxis"},

{"anaphylaxis", "Anafylaxe – těžká alergická reakce", C::kEmergency, U::kEmergency,
 "anafylaxe alergie alergicka reakce otok jazyka otok rtu duseni kopřivka po bodnutí po jídle oriesky adrenalin epipen",
 "Otok jazyka nebo hrdla, chrapot, dušnost, pískání, slabost, závrať, bledost, bezvědomí po kontaktu s alergenem.",
 "Má-li adrenalinové pero (EpiPen, Jext, Emerade): ihned ho použij do zevní strany stehna (i přes oblečení), drž 3–10 s podle návodu.\n"
  "Dušnost: posaď ho. Slabost, bledost: polož ho a zvedni mu nohy. Bezvědomí s dechem: zotavovací poloha.\n"
 "Bez zlepšení za 5 minut: druhé pero, má-li ho.\n"
 "Nedýchá: resuscitace.\n"
 "Antihistaminikum (cetirizin, loratadin) dej hned, jakmile polyká – pomůže na kopřivku a otok, ale adrenalin nenahradí.\n"
 "Reakce se může za pár hodin vrátit: hlídej ho do druhého dne.",
 "Nenech ho stát nebo chodit, ani když se mu uleví.\n"
 "Nečekej, jestli to přejde.",
 "cetirizine loratadine", "", "insect_sting allergy cpr"},

{"hypoglycemia", "Cukrovka – nízký cukr", C::kEmergency, U::kDoctor,
 "cukrovka diabetes hypoglykemie nizky cukr inzulin tres pot zmatenost hlad",
 "Je zmatený a nemůže bezpečně polykat, je v bezvědomí, nebo se do 15 minut nezlepší.",
 "Je při vědomí a umí polykat: dej mu 15–20 g cukru – 4–5 kostek cukru, sklenici slazeného nápoje nebo džusu, glukózové tablety.\n"
 "Za 15 minut se nezlepší: dej znovu.\n"
 "Až se zlepší, ať sní něco s pomalými sacharidy (chléb, sušenka).\n"
 "Bezvědomí: zotavovací poloha, nic do úst.",
 "Nedávej jídlo ani pití člověku v bezvědomí.\n"
 "Nedávej inzulin.",
 "", "", "unconscious"},

{"drowning", "Tonutí", C::kEmergency, U::kEmergency,
 "tonuti tonutí utopil voda vytazeny z vody nedycha",
 "Kašel, dušnost, zmatenost i hodiny po vytažení – voda v plicích může dýchání zhoršit. I když se zdá v pořádku, co nejdřív k lékaři.",
 "Nepouštěj se do vody, pokud nejsi vycvičený – podej tyč, lano, plovoucí předmět.\n"
 "Na břehu zkontroluj dech. Nedýchá: začni 5 vdechy, pak resuscitace 30:2.\n"
 "Dýchá: zotavovací poloha, přikryj, zahřej a do druhého dne ho hlídej.\n"
 "Zvrací při resuscitaci: otoč ho na bok, vyčisti ústa, pokračuj.",
 "Nesnaž se vylévat vodu z plic, ztrácíš čas.\n"
 "Neskákej do neznámé vody pro tonoucího.",
 "", "", "cpr hypothermia"},

{"electric", "Úraz elektrickým proudem", C::kEmergency, U::kEmergency,
 "elektricky proud uraz proudem zasah elektrinou kopnul proud dráty",
 "Bezvědomí, popáleniny, nepravidelný tep po úrazu proudem – i když se zdá v pořádku, srdce se může rozbušit později: hlídej ho a co nejdřív k lékaři.",
 "Nejdřív vypni proud (jistič, vytáhni zástrčku). Nesahej na člověka, dokud je pod proudem.\n"
 "Vysoké napětí (dráty venku, železnice): drž se aspoň 20 m daleko, dokud vedení není vypnuté.\n"
 "Nedýchá: resuscitace. Dýchá: zotavovací poloha.\n"
 "Popáleniny (vstup a výstup proudu) chlaď vodou a překryj sterilně.",
 "Neodtahuj ho holýma rukama ani kovovým předmětem.",
 "", "", "cpr burns"},

// ── Úrazy a rány ────────────────────────────────────────────────────────────
{"wound_abrasion", "Odřenina", C::kInjury, U::kSelfCare,
 "odrenina odřenina odrene koleno sedrene drobne poraneni kuze",
 "Rána je hluboká, velká, je v ní nečistota, kterou nejde vymýt, nebo se objeví známky infekce (zarudnutí, otok, hnis, horečka).",
 "Umyj si ruce.\n"
 "Opláchni ránu čistou pitnou vodou nebo fyziologickým roztokem, klidně několik minut. Nečistoty vymyj, drobné vyber pinzetou.\n"
 "Okolí rány očisti mýdlem a vodou.\n"
 "Vydezinfikuj antiseptikem (oktenidin, povidon-jód, chlorhexidin).\n"
 "Překryj nepřilnavým krytím nebo náplastí. Velkou plochu nech pod krytím vlhkou – hojí se rychleji.\n"
 "Kryt měň denně nebo když se namočí. Zkontroluj očkování proti tetanu (každých 10–15 let).",
 "Nepoužívej vatu přímo na ránu.\n"
 "Nelij na ránu peroxid ani alkohol – poškozují tkáň.",
 "octenidine povidone_iodine chlorhexidine panthenol",
 "Calendula officinalis|Měsíček lékařský|mast nebo obklad z odvaru (1 lžíce květů na 250 ml vody) na čistou, už nekrvácející odřeninu\n"
 "Plantago lanceolata|Jitrocel kopinatý|v nouzi čistý list rozdrť a přilož na drobnou odřeninu, převaž; jen dokud nemáš lepší krytí",
 "wound_cut tetanus bandage_basics"},

{"wound_cut", "Řezná a tržná rána", C::kInjury, U::kDoctor,
 "rezna rana řezná rána tržná pořezal noz sklo rana ze ktere tece krev sit seti",
 "Silné krvácení, které nejde zastavit, rána přes 1 cm hluboká nebo zející (bude třeba šít – do 6–8 hodin), na obličeji, v dlani, nad kloubem, nebo nejde hýbat prsty.",
 "Zastav krvácení tlakem přes čistou látku (5–10 minut).\n"
 "Opláchni ránu pitnou vodou.\n"
 "Vydezinfikuj okolí.\n"
 "Malou ránu stáhni náplasťovými stehy (steri-strip) napříč ránou, pak překryj.\n"
 "Zející rána: sterilní krytí, obvaz a k lékaři k sešití.\n"
 "Zvedni končetinu nad úroveň srdce, pomáhá to proti krvácení i otoku.",
 "Nesypej do rány nic (zásyp, mouku, popel).\n"
 "Neodstraňuj zabodnuté předměty.",
 "octenidine povidone_iodine",
 "",
 "bleeding wound_abrasion bandage_pressure tetanus"},

{"wound_puncture", "Bodná rána, zabodnutý předmět", C::kInjury, U::kDoctor,
 "bodna rana zabodnuty predmet nuz hrebik pichnuti zarazeny kus dreva",
 "Hluboká bodná rána do hrudníku, břicha, krku nebo hlavy, nebo zabodnutý předmět – ohrožení života.",
 "Zabodnutý předmět nevytahuj – drží cévy zavřené.\n"
 "Obal ho a obvaž kolem něj tak, aby se nehýbal (např. srolované obvazy z obou stran).\n"
 "Hrudník: rána sání vzduchu – přelož ji, ale nelep vzduchotěsně ze všech stran; nech ho v polosedu.\n"
 "Malá bodná rána (hřebík): nech krátce krvácet, vymyj, vydezinfikuj, překryj a hlídej infekci. Tetanus!",
 "Nevytahuj nůž, tyč ani sklo.\n"
 "Nedávej pít ani jíst.",
 "", "", "bleeding tetanus"},

{"animal_bite", "Kousnutí zvířetem", C::kInjury, U::kDoctor,
 "kousnuti zvire pes kocka kousl liska netopyr vzteklina",
 "Vždy k lékaři při kousnutí do obličeje, ruky, hluboko, od divokého nebo neznámého zvířete (vzteklina), a když kousla kočka (časté infekce).",
 "Ránu důkladně vymyj vodou a mýdlem aspoň 15 minut – to je nejlepší ochrana proti vzteklině.\n"
 "Vydezinfikuj (povidon-jód).\n"
 "Překryj a k lékaři – kousnutí se většinou nešijí a často dostaneš antibiotika.\n"
 "Zjisti, čí je zvíře a zda je očkované proti vzteklině. Divoké zvíře nebo netopýr: k lékaři ještě týž den.",
 "Nezalepuj kousnutí vzduchotěsně.",
 "povidone_iodine octenidine",
 "", "tetanus wound_cut"},

{"tetanus", "Tetanus – kdy přeočkovat", C::kInjury, U::kDoctor,
 "tetanus ockovani preockovani zarazeny hrebik rana od hliny",
 "Ztuhlost čelisti, křeče svalů po poranění: ihned k lékaři.",
 "Tetanus vzniká z hlíny, prachu a rezu v ráně.\n"
 "Dospělí se přeočkovávají každých 10–15 let.\n"
 "Je rána znečištěná a poslední očkování je přes 10 let (nebo nevíš): do 24 hodin k lékaři.",
 "", "", "", "wound_abrasion wound_puncture"},

{"fracture", "Zlomenina", C::kInjury, U::kDoctor,
 "zlomenina zlomena noha ruka kost deformace nemuze stoupnout",
 "Otevřená zlomenina, zlomenina stehna, pánve, páteře nebo lebky, velká deformace, končetina pod zlomeninou je studená, bledá nebo necitlivá – co nejdřív k lékaři.",
 "Nehýbej zlomenou částí, nech ji v poloze, ve které je.\n"
 "Otevřená zlomenina: zakryj ránu sterilně, zastav krvácení tlakem kolem kosti, ne na ni.\n"
 "Znehybni: dlaha (desky, klacky, srolované noviny) přes oba sousední klouby, obvázat, ne příliš těsně. Ruku do šátkového závěsu.\n"
 "Chlaď přes látku (led v sáčku) po 20 minutách.\n"
 "Po znehybnění kontroluj prsty: barva, teplota, citlivost. Bledé, studené: povol obvaz.\n"
 "Bolest: paracetamol nebo ibuprofen.",
 "Nenapravuj zlomeninu.\n"
 "Nedávej jíst ani pít (může jít na operaci).",
 "paracetamol ibuprofen",
 "Symphytum officinale|Kostival lékařský|mast nebo obklad jen na zavřené pohmožděniny a po zhojení zlomeniny, ne na otevřené rány, max. 10 dní",
 "splint sling shock"},

{"sprain", "Podvrtnutí, výron (kotník, zápěstí)", C::kInjury, U::kSelfCare,
 "podvrtnuti vyron kotnik vymknuty kotnik natazeny sval otok kloubu",
 "Nejde na nohu vůbec stoupnout ani udělat 4 kroky, kost je bolestivá na pohmat, deformace nebo bolest neustupuje do týdne.",
 "Klid: zatěžuj jen tolik, kolik bolest dovolí.\n"
 "Led: chlaď přes látku 15–20 minut každé 2–3 hodiny první 2 dny.\n"
 "Komprese: pružný obvaz od prstů nahoru, ne příliš těsně.\n"
 "Zvednout nad úroveň srdce.\n"
 "Bolest a otok: ibuprofen tablety nebo gel (diklofenak).\n"
 "Po 2–3 dnech začni opatrně hýbat.",
 "Neohřívej a nemasíruj první 2 dny.\n"
 "Nepřikládej led přímo na kůži.",
 "ibuprofen diclofenac_gel paracetamol",
 "Symphytum officinale|Kostival lékařský|mast na nepoškozenou kůži nad výronem 2–3× denně, max. 10 dní\n"
 "Arnica montana|Prha arnika|gel nebo mast na pohmožděninu, ne na otevřenou ránu",
 "fracture bandage_ankle"},

{"dislocation", "Vykloubení", C::kInjury, U::kDoctor,
 "vykloubeni vymknuti ramene vykloubene rameno prst kloub mimo",
 "Končetina pod kloubem je studená, bledá nebo necitlivá – co nejdřív k lékaři.",
 "Znehybni kloub v poloze, ve které je (rameno: šátkový závěs a podložit).\n"
 "Chlaď přes látku.\n"
 "Bolest: paracetamol nebo ibuprofen.\n"
 "Odvez k lékaři.",
 "Nenapravuj sám – můžeš poškodit nervy a cévy.",
 "paracetamol ibuprofen", "", "sling fracture"},

{"head_injury", "Úraz hlavy, otřes mozku", C::kInjury, U::kDoctor,
 "uraz hlavy otres mozku uder do hlavy bouchl se do hlavy padl na hlavu boule",
 "Bezvědomí i krátké, zvracení, zmatenost, ospalost, silná bolest hlavy, různě velké zornice, tekutina z nosu nebo uší, křeče, slabost končetin, u dětí neobvyklé chování – krvácení do mozku, co nejdřív k lékaři.",
 "Posaď ho a přilož studený obklad.\n"
 "Krvácí-li hlava, tlač na ránu – hlava krvácí silně, ale většinou to není nebezpečné.\n"
 "Bezvědomí: kontroluj dech, při podezření na páteř hýbej hlavou co nejméně; dýchá-li, zotavovací poloha jen pokud musíš odejít nebo zvrací.\n"
 "Po úrazu ho 24 hodin sleduj, budit ho v noci každé 2–3 hodiny a ověřit, že normálně reaguje.\n"
 "Bolest: paracetamol.",
 "Nedávej ibuprofen ani aspirin prvních 24 hodin (riziko krvácení).\n"
 "Nedovol mu řídit a pít alkohol.",
 "paracetamol", "", "spine unconscious"},

{"spine", "Podezření na poranění páteře", C::kInjury, U::kEmergency,
 "pater páteř zada krk upadl z vysky nehoda skok do vody necitlive nohy brneni",
 "Pád z výšky, skok do mělké vody, dopravní nehoda, bolest krku nebo zad, brnění či necitlivost končetin.",
 "Řekni mu, ať se nehýbe. Drž mu hlavu oběma rukama v poloze, ve které je.\n"
 "Hýbej s ním jen když hrozí nebezpečí (požár, voda) nebo nedýchá.\n"
 "Nedýchá: polož ho na záda co nejšetrněji (ve více lidech, hlava–krk–trup v jedné ose) a resuscituj.",
 "Nezkoušej, jestli hýbe nohama.\n"
 "Nezvedej ho, neposazuj.",
 "", "", "cpr head_injury"},

{"chest_injury", "Poranění hrudníku", C::kInjury, U::kEmergency,
 "hrudnik poraneni zebra zlomena zebra rana do hrudi dusnost po uraze",
 "Dušnost, rána do hrudníku, vykašlávání krve, zlomená žebra u starších lidí – vážné.",
 "Posaď ho do polosedu, nakloněného na poraněnou stranu.\n"
 "Otevřenou ránu, která sípe, překryj (fólie, obal) a přilep jen ze tří stran, aby vzduch mohl ven.\n"
 "Zlomená žebra neobvazuj kolem dokola.\n"
 "Sleduj dech, připrav se na resuscitaci.",
 "Nevytahuj zabodnuté předměty.",
 "", "", "wound_puncture shock"},

{"abdominal_injury", "Poranění břicha", C::kInjury, U::kEmergency,
 "bricho poraneni rana do bricha uder do bricha vyhrezle strevo",
 "Každé pronikající poranění břicha ohrožuje život.",
 "Polož ho na záda s pokrčenými koleny (podložit).\n"
 "Vyhřezlé střevo nezatlačuj zpět – překryj vlhkým čistým krytím (fólie, vlhká látka).\n"
 "Hlídej šok, přikryj.",
 "Nedávej jíst ani pít.",
 "", "", "shock"},

{"amputation", "Utržená část těla (prst, končetina)", C::kInjury, U::kEmergency,
 "amputace utrzeny prst useknuty prst urizl si prst",
 "Vždy – hlavní je zastavit krvácení.",
 "Zastav krvácení tlakem, u končetiny případně škrtidlem.\n"
 "Utrženou část zabal do suché sterilní gázy, dej do sáčku, sáček zavaž a vlož do dalšího sáčku s vodou a ledem.\n"
 "Vezmi ji se zraněným k lékaři – přišít se dá do 6–12 hodin.\n"
 "Hlídej šok.",
 "Utrženou část nedávej přímo na led ani do vody.\n"
 "Nečisti ji.",
 "", "", "bleeding shock"},

{"nosebleed", "Krvácení z nosu", C::kInjury, U::kSelfCare,
 "krvaceni z nosu krev z nosu tece z nosu",
 "Krvácí déle než 20–30 minut, po úrazu hlavy, nebo velmi silně, u lidí na lécích na ředění krve.",
 "Posaď ho a předkloň hlavu.\n"
 "Stiskni měkkou část nosu (pod kostí) palcem a ukazovákem 10–15 minut bez povolení.\n"
 "Ať dýchá ústy a krev vyplivuje.\n"
 "Studený obklad na kořen nosu a zátylek.\n"
 "Po zastavení pár hodin nesmrkat, nezvedat těžké věci, nepít horké.",
 "Nezakláněj hlavu (krev teče do žaludku a vyvolá zvracení).\n"
 "Neucpávej nos hluboko vatou.",
 "xylometazoline", "", ""},

{"tooth", "Vyražený zub", C::kInjury, U::kDoctor,
 "vyrazeny zub vypadl zub zlomeny zub uraz zubu",
 "K zubaři do 30–60 minut – trvalý zub jde často přišít.",
 "Zub chytej za korunku, ne za kořen.\n"
 "Špinavý jen krátce opláchni mlékem nebo slinou (ne vodou, nedrhnout).\n"
 "Trvalý zub zkus vrátit do jamky a skousnout do kapesníku.\n"
 "Nejde to: přenášej ho v mléce nebo ve slinách (v ústech u tváře – ne u malého dítěte).\n"
 "Mléčný zub nevracej.",
 "Nenech zub uschnout.",
 "paracetamol ibuprofen", "", "toothache"},

{"eye_foreign", "Cizí těleso v oku", C::kInjury, U::kDoctor,
 "oko cizi teleso v oku smitko v oku pisek v oku zabodnute v oku",
 "Zabodnuté těleso, poranění oka, zhoršené vidění: zakryj obě oči a k lékaři.",
 "Nemnout oko.\n"
 "Vyplach: hlavu nakloň na stranu oka, lij čistou vodu od vnitřního koutku ven několik minut.\n"
 "Smítko na víčku vyndej rohem čistého kapesníku.\n"
 "Pocit cizího tělesa trvá: k očnímu lékaři.",
 "Nevytahuj zabodnuté předměty.",
 "", "", "eye_chemical"},

{"eye_chemical", "Poleptání oka, chemikálie v oku", C::kInjury, U::kEmergency,
 "polepteni oka chemikalie v oku louh vapno kyselina sprej do oci",
 "Louh a vápno jsou nejhorší – vyplachuj dlouho, pak k lékaři.",
 "Okamžitě vyplachuj proudem vlažné vody aspoň 15–20 minut, oko drž otevřené.\n"
 "Vyplachuj od nosu ven, aby voda netekla do druhého oka.\n"
 "Vyndej kontaktní čočky.\n"
 "Vezmi s sebou obal chemikálie.",
 "Neneutralizuj (ocet, soda).",
 "", "", "chemical_burn"},

{"splinter", "Tříska, osten", C::kInjury, U::kSelfCare,
 "triska tříska osten trn ve kuzi zarazeny trn",
 "Velká nebo hluboká tříska, pod nehtem, v oku, nebo zánět.",
 "Umyj ruce a místo mýdlem.\n"
 "Vydezinfikuj pinzetu.\n"
 "Chyť třísku co nejblíž u kůže a vytáhni ve směru, kterým vnikla.\n"
 "Hluboká: změkčit 10 minut v teplé vodě, pak jehlou opatrně odkrýt konec.\n"
 "Vydezinfikuj a překryj náplastí.",
 "Nemačkej kolem – tříska se zlomí.",
 "octenidine povidone_iodine", "", "wound_abrasion"},

{"blister", "Puchýř", C::kInjury, U::kSelfCare,
 "puchyr puchýř odrene paty puchyr na noze bota",
 "Puchýř je zanícený (zarudlý, teplý, hnis) nebo u diabetika.",
 "Malý neporušený puchýř nepropichuj, přelep náplastí na puchýře nebo polštářkem.\n"
 "Velký bolestivý: vydezinfikuj, propíchni sterilní jehlou u okraje, vymačkej tekutinu, kůži nech na místě.\n"
 "Překryj (hydrokoloidní náplast) a ulev tlaku (jiná bota, polstrování).",
 "Nestrhávej kůži z puchýře.",
 "octenidine", "", "burns"},

{"burns", "Popálenina", C::kInjury, U::kDoctor,
 "popalenina popálenina opareni opaření spaleni horka voda ohen zehlicka puchyr",
 "Popálenina větší než dlaň postiženého, na obličeji, rukou, genitálu, kloubech, hluboká (bílá, hnědá, necitlivá), u dětí a starších, vdechnutí kouře, chemická nebo elektrická – vážná popálenina.",
 "Okamžitě chlaď vlažnou tekoucí vodou (15–25 °C) 20 minut. Platí i do 3 hodin po popálení.\n"
 "Během chlazení sundej prstýnky, hodinky, oblečení (co nepřilnulo).\n"
 "Pak překryj volně čistou fólií (potravinová fólie, neutahovat) nebo sterilním nepřilnavým krytím.\n"
 "Malou popáleninu (zarudnutí, malé puchýře) ošetři panthenolem a nech volnou nebo volně překrytou.\n"
 "Bolest: paracetamol nebo ibuprofen.\n"
 "Velká plocha: hlídej podchlazení a šok – chlaď jen popáleninu, ne celé tělo.",
 "Nepoužívej led, máslo, olej, mouku, zubní pastu.\n"
 "Nestrhávej přiškvařený oděv.\n"
 "Nepropichuj puchýře.",
 "panthenol paracetamol ibuprofen",
 "Aloe vera|Aloe pravá|čerstvý gel z listu na malou, už zchlazenou popáleninu bez puchýřů\n"
 "Calendula officinalis|Měsíček lékařský|mast až na hojící se drobnou popáleninu",
 "chemical_burn sunburn shock"},

{"chemical_burn", "Poleptání kůže", C::kInjury, U::kDoctor,
 "polepteni kuze chemikalie louh kyselina vapno cistic",
 "Velká plocha, obličej, oči, louh, nebo bolest neustupuje – vážné.",
 "Chraň se rukavicemi.\n"
 "Suchou chemikálii (vápno) nejdřív smeť, pak oplachuj.\n"
 "Oplachuj velkým množstvím vody aspoň 20 minut, sundej zasažené oblečení.\n"
 "Překryj sterilně. Vezmi obal chemikálie k lékaři.",
 "Neneutralizuj kyselinu zásadou ani obráceně.",
 "", "", "eye_chemical burns"},

{"sunburn", "Spálení od slunce", C::kEnvironment, U::kSelfCare,
 "spaleni od slunce spalena kuze opaleni zarudnuti po slunci",
 "Rozsáhlé puchýře, horečka, zimnice, zvracení, zmatenost, u malých dětí.",
 "Odejdi do stínu.\n"
 "Chlaď kůži vlažnou sprchou nebo obklady.\n"
 "Panthenol nebo hydratační mléko (bez parfému).\n"
 "Hodně pij.\n"
 "Bolest: ibuprofen nebo paracetamol.\n"
 "Pár dní nechoď na slunce.",
 "Nepropichuj puchýře.\n"
 "Nemaž mastnými krémy ani máslem.",
 "panthenol ibuprofen paracetamol",
 "Aloe vera|Aloe pravá|gel z listu tence na spálenou kůži 2–3× denně",
 "heatstroke burns"},

// ── Obvazy a dlahy ──────────────────────────────────────────────────────────
{"bandage_basics", "Jak obvazovat – základ", C::kBandage, U::kSelfCare,
 "obvaz jak obvazat obvazovani krytí naplast obinadlo",
 "",
 "Umyj si ruce, natáhni rukavice.\n"
 "Na ránu vždy nejdřív sterilní krytí (čtverec gázy, polštářek obvazu) – nikdy obinadlo přímo.\n"
 "Obinadlo veď od užšího místa k širšímu (od prstů k tělu), každá otáčka překrývá předchozí z poloviny nebo dvou třetin.\n"
 "Začni a skonči dvěma kruhovými otáčkami, konec přichyť náplastí nebo zavaž.\n"
 "Obvaz musí držet, ale nesmí škrtit: prsty zůstanou růžové, teplé a citlivé. Zbělají nebo brní: povol.\n"
 "Na kloub dělej osmičky (kotník, loket, koleno).",
 "Neutahuj obvaz na ruce nebo noze tak, že nejde nehet stisknutím zrůžovět do 2 s.",
 "", "", "bandage_pressure bandage_head bandage_finger bandage_ankle sling"},

{"bandage_pressure", "Tlakový obvaz", C::kBandage, U::kEmergency,
 "tlakovy obvaz zastavit krvaceni obvaz na krvaceni",
 "Krvácení prosakuje i přes tlakový obvaz: tlač rukou dál; krvácení z končetiny zastav škrtidlem.",
 "Na ránu polož sterilní krytí (polštářek obvazu).\n"
 "Na něj tlakovou vrstvu – srolované obinadlo nebo jiný tvrdší předmět velikosti rány.\n"
 "Pevně převaž několika otáčkami přes tlakovou vrstvu.\n"
 "Prosákne-li, nepovoluj, přiložit druhou tlakovou vrstvu a převázat.\n"
 "Zkontroluj prokrvení prstů pod obvazem.",
 "Nesundávej první vrstvu, když prosákne.",
 "", "", "bleeding bandage_basics"},

{"bandage_head", "Obvaz hlavy", C::kBandage, U::kSelfCare,
 "obvaz hlavy obvazat hlavu rana na hlave",
 "",
 "Na ránu sterilní krytí.\n"
 "Začni kruhovými otáčkami kolem hlavy přes čelo a týl, pod týlním hrbolem.\n"
 "Pak střídej otáčky přes temeno dopředu a dozadu, které chytáš kruhovými otáčkami (čepicový obvaz), nebo použij síťový obvaz.\n"
 "Nepřekrývej oči a uši, je-li to možné.",
 "", "", "", "head_injury bandage_basics"},

{"bandage_finger", "Obvaz prstu a ruky", C::kBandage, U::kSelfCare,
 "obvaz prstu obvazat prst ruka dlan",
 "",
 "Prst: krytí, pak obinadlo vést od špičky k bázi spirálou, přes hřbet ruky zpět kolem zápěstí a uvázat.\n"
 "Dlaň: do dlaně srolovaný polštářek, ať ruka svírá, osmičkami přes dlaň a zápěstí.\n"
 "Konečky prstů nech volné pro kontrolu prokrvení.",
 "", "", "", "bandage_basics"},

{"bandage_ankle", "Obvaz kotníku a kolena (osmička)", C::kBandage, U::kSelfCare,
 "obvaz kotniku obvazat kotnik koleno osmickovy obvaz pruzne obinadlo",
 "",
 "Chodidlo v pravém úhlu.\n"
 "Začni dvěma kruhy kolem nártu u prstů.\n"
 "Veď obinadlo šikmo přes nárt nad kotník, kolem kotníku a zpět přes nárt pod chodidlo – osmičky.\n"
 "Každá osmička o kousek výš, skonči kruhem nad kotníkem.\n"
 "Koleno: mírně pokrčené, osmičky pod a nad kolenem, překřížení v podkolenní jamce.",
 "Pružné obinadlo nenatahuj na doraz.",
 "", "", "sprain bandage_basics"},

{"sling", "Šátkový závěs", C::kBandage, U::kSelfCare,
 "satkovy zaves zaves ruky zlomena ruka trojcipy satek",
 "",
 "Trojcípý šátek (nebo tričko, šála) dej pod poraněnou ruku, špička k lokti.\n"
 "Horní cíp přes zdravé rameno, dolní cíp přes ruku nahoru k poraněnému rameni.\n"
 "Svaž cípy na boku krku (ne na páteři), ruka v pravém úhlu, dlaň mírně výš.\n"
 "Špičku u lokte přichyť zavázáním nebo zatočením.\n"
 "Pro zpevnění přidej druhý šátek kolem hrudníku přes paži.",
 "", "", "", "fracture dislocation"},

{"splint", "Dlaha, znehybnění", C::kBandage, U::kDoctor,
 "dlaha znehybneni dlahovani zlomenina provizorni dlaha",
 "",
 "Dlaha musí přesahovat oba sousední klouby zlomené kosti.\n"
 "Použij, co máš: prkénka, klacky, karimatku, srolované noviny, deštník.\n"
 "Vyplň mezery a místa u kloubů měkkým materiálem.\n"
 "Přivaž nad a pod zlomeninou, ne přímo přes ni.\n"
 "Noha: lze přivázat ke zdravé noze s polstrováním mezi koleny a kotníky.\n"
 "Po znehybnění kontroluj prsty.",
 "Nenapravuj úhel zlomeniny.",
 "", "", "fracture sling"},

// ── Nemoci a příznaky ───────────────────────────────────────────────────────
{"fever", "Horečka", C::kIllness, U::kSelfCare,
 "horecka horečka teplota zvysena teplota zimnice tepla",
 "Dítě do 3 měsíců s teplotou nad 38 °C, horečka nad 40 °C, ztuhlý krk, vyrážka, která nezmizí při stlačení sklenicí, dušnost, zmatenost, křeče, horečka déle než 3 dny, nebo nepije a nemočí.",
 "Změř teplotu. Horečka je od 38 °C.\n"
 "Hodně pít (voda, čaj, vývar), odpočívat.\n"
 "Lehce oblékni, nepřikrývej teple.\n"
 "Lék na horečku podle váhy, je-li nepříjemná nebo nad 38,5 °C: paracetamol nebo ibuprofen.\n"
 "Chladivý obklad na čelo, vlažná (ne studená) sprcha.",
 "Dětem a mladistvým nedávej aspirin (Reyeův syndrom).\n"
 "Nestřídej léky bez potřeby, dodržuj odstupy.",
 "paracetamol ibuprofen",
 "Tilia cordata|Lípa srdčitá|čaj z květu: 1–2 lžičky na 250 ml vroucí vody, 10 min, 2–3× denně, podporuje pocení\n"
 "Sambucus nigra|Bez černý|čaj ze sušeného květu: 1 lžíce na 250 ml, 10 min, 2–3× denně (syrové bobule a listy jsou jedovaté)",
 "cold febrile_seizure dehydration"},

{"cough", "Kašel", C::kIllness, U::kSelfCare,
 "kasel kašel kaslani suchy kasel vlhky kasel hlen odkasavani nachlazeni",
 "Dušnost, vykašlávání krve, horečka déle než 3 dny, bolest na hrudi, kašel déle než 3 týdny, u kojence, nebo pískot při dýchání.",
 "Pij hodně tekutin (teplý čaj s medem – med ne dětem do 1 roku).\n"
 "Zvlhčuj vzduch, inhaluj páru (miska horké vody, ručník přes hlavu, 10 minut; ne malým dětem).\n"
 "Vlhký kašel s hlenem: lék na odkašlávání ráno a přes den (ambroxol, acetylcystein).\n"
 "Suchý dráždivý kašel: lék proti kašli na noc (butamirát, dextromethorfan).\n"
 "Spi se zvýšenou hlavou.",
 "Nekombinuj lék na odkašlávání s lékem proti kašli zároveň (hlen by zůstal v plicích).\n"
 "Med nedávej dětem do 1 roku.",
 "ambroxol acetylcysteine butamirate dextromethorphan",
 "Thymus vulgaris|Tymián obecný|čaj: 1 lžička na 250 ml vroucí vody, 10 min přikrýt, 3× denně, s medem\n"
 "Plantago lanceolata|Jitrocel kopinatý|čaj z listu: 1–2 lžičky na 250 ml, 10 min, 3× denně; nebo sirup\n"
 "Tilia cordata|Lípa srdčitá|čaj z květu na dráždivý kašel, 2–3× denně\n"
 "Althaea officinalis|Proskurník lékařský|studený výluh z kořene: 1 lžička na 250 ml studené vody 1–2 h, na suchý kašel",
 "cold sore_throat fever"},

{"sore_throat", "Bolest v krku", C::kIllness, U::kSelfCare,
 "bolest v krku bolí v krku krk angina skrabani pálení v krku",
 "Nemůže polykat sliny, otevřít ústa, dýchá ztěžka, vysoká horečka, otok krku, nebo bolest trvá déle než týden.",
 "Pij teplé tekutiny, cucej pastilky.\n"
 "Kloktej slanou vodou (lžička soli na sklenici) nebo šalvějí několikrát denně.\n"
 "Bolest: paracetamol nebo ibuprofen.\n"
 "Odpočívej, zvlhčuj vzduch.",
 "",
 "paracetamol ibuprofen",
 "Salvia officinalis|Šalvěj lékařská|kloktadlo: 1 lžíce listů na 250 ml vroucí vody, 10 min, vlažným kloktat 3–4× denně (nepít dlouhodobě, ne v těhotenství)\n"
 "Matricaria chamomilla|Heřmánek pravý|čaj na kloktání i pití, 2–3× denně",
 "cold fever"},

{"cold", "Rýma a nachlazení", C::kIllness, U::kSelfCare,
 "ryma rýma nachlazeni nachlazení ucpany nos smrkani virova infekce chripka",
 "Vysoká horečka přes 3 dny, silná bolest hlavy nebo dutin, dušnost, zhoršení po zlepšení.",
 "Odpočívej, hodně pij.\n"
 "Nos vyplachuj solným roztokem (mořská voda).\n"
 "Ucpaný nos: nosní kapky s xylometazolinem na noc, nejvýš 5–7 dní.\n"
 "Bolest, horečka: paracetamol nebo ibuprofen.\n"
 "Inhalace páry.",
 "Nepoužívej nosní kapky na ucpaný nos déle než týden – nos si na ně zvykne.",
 "xylometazoline paracetamol ibuprofen",
 "Sambucus nigra|Bez černý|čaj z květu 2–3× denně\n"
 "Tilia cordata|Lípa srdčitá|čaj z květu 2–3× denně\n"
 "Urtica dioica|Kopřiva dvoudomá|čaj z listu (vitaminy, minerály), 2× denně",
 "cough fever sore_throat"},

{"diarrhea", "Průjem", C::kIllness, U::kSelfCare,
 "prujem průjem ridka stolice zaludecni chripka strevni chripka",
 "Krev ve stolici, vysoká horečka, silné bolesti břicha, známky odvodnění (nemočí, suchá ústa, malátnost), u kojence a starého člověka, nebo trvá déle než 3 dny.",
 "Hlavní je pít: rehydratační roztok po malých doušcích, i když zvrací.\n"
 "Domácí roztok: 1 litr převařené vody, 6 zarovnaných lžiček cukru a půl lžičky soli.\n"
 "Dospělý vypije po každé řídké stolici 250–500 ml navíc, dítě 10 ml na kg váhy.\n"
 "Jez lehce: rýže, banán, suchar, mrkev, vývar.\n"
 "Živočišné uhlí může pomoct. Loperamid jen dospělým a jen bez horečky a krve ve stolici.\n"
 "Myj si ruce – průjem je nakažlivý.",
 "Nedávej loperamid dětem do 12 let ani při horečce či krvi ve stolici.\n"
 "Nepij sladké limonády a mléko.",
 "ors activated_charcoal loperamide",
 "Vaccinium myrtillus|Borůvka černá|sušené plody: rozžvýkat 1–2 lžíce, nebo odvar, 3× denně\n"
 "Agrimonia eupatoria|Řepík lékařský|čaj: 1–2 lžičky na 250 ml, 10 min, 3× denně\n"
 "Camellia sinensis|Čajovník|silný černý čaj louhovaný 10 min",
 "vomiting dehydration food_poisoning"},

{"vomiting", "Zvracení, nevolnost", C::kIllness, U::kSelfCare,
 "zvraceni zvracení nevolnost bliti blinkat nevolno",
 "Zvratky s krví nebo jako kávová sedlina, silná bolest břicha, po úrazu hlavy, nemůže udržet vůbec žádnou tekutinu přes 12 h (dítě 6 h), známky odvodnění, ztuhlý krk.",
 "Hodinu po zvracení nejez. Pak pij po lžičkách (rehydratační roztok, čaj) každých 5 minut.\n"
 "Postupně zvyšuj množství.\n"
 "Pak lehká strava.\n"
 "Odpočívej na boku, zvlášť dítě.",
 "Nepij najednou velké množství.",
 "ors",
 "Zingiber officinale|Zázvor lékařský|čaj z čerstvého kořene: plátek 2–3 cm na 250 ml, 10 min, popíjet\n"
 "Mentha × piperita|Máta peprná|čaj: 1 lžička na 250 ml, 10 min (ne kojencům)",
 "diarrhea dehydration motion_sickness"},

{"dehydration", "Odvodnění (dehydratace)", C::kIllness, U::kDoctor,
 "dehydratace odvodneni malo moci suchá ústa nepije zizen",
 "Nemočí přes 8 hodin (dítě 6), apatie, zmatenost, propadlé oči, rychlý dech, kojenec s propadlou fontanelou.",
 "Pij rehydratační roztok po malých doušcích, ne čistou vodu ve velkém.\n"
 "Domácí roztok: 1 l převařené vody, 6 lžiček cukru, půl lžičky soli.\n"
 "Dítě: 50–100 ml na kg váhy během 4 hodin.\n"
 "Odpočívej ve stínu a chladu.",
 "",
 "ors", "", "diarrhea vomiting heatstroke"},

{"food_poisoning", "Otrava jídlem", C::kIllness, U::kDoctor,
 "otrava jidlem zkazene jidlo salmonela zvraceni a prujem po jidle",
 "Poruchy vidění, polykání nebo řeči, ochrnutí (botulismus), krev ve stolici, vysoká horečka, odvodnění.",
 "Dodržuj pitný režim (rehydratační roztok).\n"
 "Živočišné uhlí.\n"
 "Odpočívej, lehká strava.\n"
 "Po jedení hub viz Otrava houbami.",
 "", "ors activated_charcoal", "", "diarrhea vomiting mushroom_poisoning"},

{"constipation", "Zácpa", C::kIllness, U::kSelfCare,
 "zacpa zácpa nemuze na stolici tvrda stolice",
 "Silná bolest břicha, zvracení, nafouklé břicho, krev ve stolici, zácpa nová a trvá přes 2 týdny.",
 "Pij víc vody, jez vlákninu (ovoce, zelenina, celozrnné pečivo, sušené švestky).\n"
 "Pohyb, chůze.\n"
 "Nespěchej na toaletě, nohy na stoličku.",
 "",
 "",
 "Linum usitatissimum|Len setý|1–2 lžíce celého semínka namočit a sníst s velkou sklenicí vody",
 ""},

{"abdominal_pain", "Bolest břicha", C::kIllness, U::kDoctor,
 "bolest bricha bolí břicho krece v brise slepe streva apendicitida",
 "Náhlá prudká bolest, tvrdé břicho, bolest vpravo dole (slepé střevo), zvracení krve, černá stolice, horečka, u těhotných, po úrazu.",
 "Odpočívej v poloze, která bolest zmírní.\n"
 "Nic nejez, pij po troškách.\n"
 "Křečovitá bolest se zažívacími potížemi: teplý čaj, teplá láhev na břicho.\n"
 "Sleduj, zda se bolest stěhuje a sílí – pak k lékaři.",
 "Nedávej projímadla a silné léky proti bolesti, dokud není jasná příčina.",
 "simethicone",
 "Matricaria chamomilla|Heřmánek pravý|čaj: 1 lžíce na 250 ml, 10 min, 3× denně\n"
 "Foeniculum vulgare|Fenykl obecný|čaj z rozdrceného semene (1 lžička na 250 ml) na nadýmání\n"
 "Carum carvi|Kmín kořenný|čaj z rozdrceného semene na nadýmání a křeče",
 "diarrhea vomiting heartburn"},

{"heartburn", "Pálení žáhy", C::kIllness, U::kSelfCare,
 "paleni zahy zaha kyselo v ustech refluxe palení za hrudní kostí",
 "Bolest na hrudi vystřeluje do paže či čelisti (může jít o infarkt), potíže s polykáním, hubnutí, zvracení krve.",
 "Jez menší porce, ne pozdě večer.\n"
 "Vynech kávu, alkohol, tučné a kořeněné.\n"
 "Spi se zvýšenou hlavou.\n"
 "Antacidum nebo lék na snížení kyseliny podle příbalového letáku.",
 "", "antacid", "", "heart_attack abdominal_pain"},

{"headache", "Bolest hlavy", C::kIllness, U::kSelfCare,
 "bolest hlavy hlava migrena bolí hlava",
 "Náhlá nejhorší bolest v životě, po úrazu, s horečkou a ztuhlým krkem, s poruchou řeči, vidění, slabostí, zmateností – vážné, co nejdřív k lékaři.",
 "Napij se, odpočiň v klidu a tmě.\n"
 "Studený obklad na čelo nebo krk.\n"
 "Lék: paracetamol nebo ibuprofen.\n"
 "Častá bolest hlavy: k lékaři, a neber léky víc než 10 dní v měsíci.",
 "",
 "paracetamol ibuprofen acetylsalicylic",
 "Mentha × piperita|Máta peprná|mátový olej zředěný tence na spánky (ne dětem do 6 let)\n"
 "Melissa officinalis|Meduňka lékařská|čaj na bolest hlavy z napětí: 1–2 lžičky na 250 ml, 10 min",
 "stroke"},

{"toothache", "Bolest zubu", C::kIllness, U::kDoctor,
 "bolest zubu bolí zub zub otok dasne zanet zubu",
 "Otok tváře, horečka, potíže s otevřením úst nebo polykáním.",
 "Lék: ibuprofen (nejlépe) nebo paracetamol.\n"
 "Chladný obklad zvenčí na tvář.\n"
 "Vyplachuj slanou vodou.\n"
 "Co nejdřív k zubaři.",
 "Nedávej aspirin přímo na dáseň.",
 "ibuprofen paracetamol",
 "Syzygium aromaticum|Hřebíček|olej na vatičku přiložit k bolavému zubu, nebo rozkousat hřebíček\n"
 "Salvia officinalis|Šalvěj lékařská|výplach úst odvarem 3–4× denně",
 "tooth"},

{"back_pain", "Bolest zad a svalů", C::kIllness, U::kSelfCare,
 "bolest zad bolí záda kriz hold namozeny sval svaly natazeny sval",
 "Necitlivost v rozkroku, nemůže močit nebo udržet stolici, slabost nohou, horečka, po úrazu – co nejdřív k lékaři.",
 "Zůstaň v pohybu, jak to jde, ne dlouho v posteli.\n"
 "Teplo (teplá sprcha, termofor) na ztuhlé svaly.\n"
 "Lék: ibuprofen nebo gel s diklofenakem.",
 "",
 "ibuprofen diclofenac_gel paracetamol",
 "Symphytum officinale|Kostival lékařský|mast na bolavé svaly a klouby 2–3× denně, max. 10 dní",
 "sprain"},

{"insomnia_anxiety", "Úzkost, panika, nespavost", C::kIllness, U::kSelfCare,
 "uzkost úzkost panika panicky zachvat nespavost nemuze spat stres bušení srdce",
 "Myšlenky na sebevraždu, bolest na hrudi, dušnost s modráním.",
 "Panika: dýchej pomalu – nádech nosem 4 s, výdech ústy 6 s, deset minut.\n"
 "Pojmenuj 5 věcí, které vidíš, 4, které slyšíš, 3, kterých se dotýkáš.\n"
 "Nespavost: pravidelný čas spánku, tma, chlad, žádný telefon hodinu před spaním.",
 "",
 "",
 "Melissa officinalis|Meduňka lékařská|čaj večer: 1–2 lžičky na 250 ml, 10 min\n"
 "Valeriana officinalis|Kozlík lékařský|čaj z kořene nebo tinktura večer (ne s alkoholem)\n"
 "Lavandula angustifolia|Levandule lékařská|čaj nebo pár kapek oleje na polštář",
 ""},

{"allergy", "Alergie, kopřivka, svědění", C::kIllness, U::kSelfCare,
 "alergie kopřivka koprivka svedeni vyrazka pyl senna ryma kytka alergie na",
 "Otok rtů, jazyka, hrdla, dušnost, slabost: anafylaxe – viz Anafylaxe.",
 "Vyhni se alergenu (pyl, zvíře, jídlo).\n"
 "Antihistaminikum: cetirizin nebo loratadin.\n"
 "Svědění kůže: chladivý obklad, hydrokortizonový krém na malou plochu.\n"
 "Sledovat dýchání.",
 "",
 "cetirizine loratadine hydrocortisone",
 "Urtica dioica|Kopřiva dvoudomá|čaj z listu při senné rýmě, 2–3× denně",
 "anaphylaxis insect_sting"},

{"urinary", "Pálení při močení (cystitida)", C::kIllness, U::kDoctor,
 "paleni pri moceni cystitida zanet mocovych cest pálí čůrání časté močení",
 "Horečka, bolest v bedrech, krev v moči, u mužů, dětí a těhotných, nebo bez zlepšení za 2 dny: k lékaři (může být třeba antibiotikum).",
 "Hodně pij (2–3 litry denně).\n"
 "Teplo na podbřišek.\n"
 "Bolest: ibuprofen.",
 "Nezadržuj moč.",
 "ibuprofen",
 "Vaccinium vitis-idaea|Brusnice brusinka|šťáva nebo plody denně\n"
 "Arctostaphylos uva-ursi|Medvědice lékařská|čaj z listu nejdéle 1 týden, ne v těhotenství a dětem",
 ""},

{"motion_sickness", "Nevolnost při cestování", C::kIllness, U::kSelfCare,
 "nevolnost pri cestovani kinetoza nevolno v aute na lodi",
 "",
 "Seď vpředu, dívej se na obzor, nečti.\n"
 "Čerstvý vzduch, malé jídlo předem.\n"
 "Lék proti kinetóze 30–60 minut před cestou podle letáku.",
 "", "dimenhydrinate",
 "Zingiber officinale|Zázvor lékařský|plátek kořene cucat nebo čaj před cestou",
 "vomiting"},

{"eczema_rash", "Vyrážka, ekzém, opruzeniny", C::kIllness, U::kSelfCare,
 "vyrazka vyrážka ekzem opruzeniny suchá kuže svědí kuze plisen",
 "Vyrážka, která nezmizí při stlačení sklenicí, s horečkou: možná meningitida – co nejdřív k lékaři. Puchýře po celém těle, otok obličeje.",
 "Kůži udržuj čistou a suchou, promašťuj neparfémovaným krémem.\n"
 "Svědění: chladivé obklady, krátce hydrokortizon.\n"
 "Opruzeniny: vysušit, zinková mast.\n"
 "Plíseň (svědění mezi prsty, v tříslech): krém s klotrimazolem 2–4 týdny.",
 "",
 "hydrocortisone clotrimazole zinc_ointment",
 "Matricaria chamomilla|Heřmánek pravý|obklad nebo koupel z odvaru na podrážděnou kůži\n"
 "Calendula officinalis|Měsíček lékařský|mast na suchou podrážděnou kůži",
 "allergy"},

// ── Otravy, kousnutí, bodnutí ───────────────────────────────────────────────
{"poisoning", "Otrava – co dělat", C::kPoison, U::kEmergency,
 "otrava otravy snědl spolkl vypil jed chemikalie tablety predavkovani leky",
 "Bezvědomí, křeče, dušnost, žíravina, nebo nevíš, co a kolik snědl – sleduj ho a co nejdřív k lékaři.",
 "Zjisti co, kolik a kdy snědl nebo vypil. Obal, zbytek, rostlinu nebo houbu vezmi s sebou.\n"
 "Do hodiny od snědení (léky, rostlina, houby – ne žíravina, benzín, alkohol ani kovy) pomůže živočišné uhlí: rozdrť tablety ve vodě, dospělý až 50 g, dítě 1 g na kg váhy.\n"
 "V ústech zbytky: vypláchni ústa vodou.\n"
  "Bezvědomí s dýcháním: zotavovací poloha.",
 "Nevyvolávej zvracení (ne při žíravinách, benzínu, saponátech!).\n"
 "Nedávej mléko ani olej.",
 "activated_charcoal", "", "plant_poisoning mushroom_poisoning co_poisoning"},

{"plant_poisoning", "Otrava rostlinou", C::kPoison, U::kEmergency,
 "otrava rostlinou snedl rostlinu bobule jedovata rostlina dite snedlo kytku list",
 "Potíže s dýcháním, křeče, bezvědomí, nepravidelný tep.",
 "Vyndej z úst zbytky rostliny a vypláchni ústa.\n"
 "Zjisti, co to bylo – rostlinu nebo foto s sebou (v aplikaci Nature ji můžeš najít).\n"
 "Do hodiny od snědení: živočišné uhlí (viz Otrava – co dělat).\n"
 "Při podráždění kůže omyj mýdlem a vodou.",
 "Nevyvolávej zvracení.\n"
 "Nečekej na příznaky – u některých rostlin přijdou za hodiny.",
 "activated_charcoal", "", "poisoning mushroom_poisoning"},

{"mushroom_poisoning", "Otrava houbami", C::kPoison, U::kEmergency,
 "otrava houbami houby muchomurka zelena snedl houby nevolno po houbach",
 "Zvracení a průjem 6 a více hodin po jídle hub znamená možnou muchomůrku zelenou – i když se pak uleví, hrozí selhání jater. Co nejdřív do nemocnice.",
 "Do hodiny od jídla: živočišné uhlí (viz Otrava – co dělat).\n"
 "Schovej zbytky jídla, odpad z čištění hub, zvratky – pomohou určit houbu.\n"
 "Zapiš, kdy jedl a kdy začaly potíže. Kdo jedl totéž, hlídej i jeho.\n"
 "Pij po troškách, při průjmu rehydratační roztok.",
 "Nečekej, až se to zlepší – u nejnebezpečnějších hub se po 1–2 dnech na čas uleví a pak selžou játra.\n"
 "Nevyvolávej zvracení bez porady.",
 "activated_charcoal ors", "", "poisoning plant_poisoning"},

{"co_poisoning", "Otrava kouřem a oxidem uhelnatým", C::kPoison, U::kEmergency,
 "otrava co oxid uhelnaty kour karma kamna bolest hlavy v mistnosti dym",
 "Bolest hlavy, závrať, nevolnost více lidí v jedné místnosti, zmatenost, bezvědomí.",
 "Vyveď lidi na čerstvý vzduch, otevři okna (sám se neotrav – zadrž dech, nevstupuj do zakouřeného prostoru).\n"
  "Bezvědomí: kontroluj dech, resuscitace.",
 "Nezapínej spotřebiče, nerozsvěcuj (riziko výbuchu u plynu).",
 "", "", "cpr"},

{"alcohol", "Opilost, otrava alkoholem", C::kPoison, U::kDoctor,
 "alkohol opily otrava alkoholem opilost vypil moc",
 "Nereaguje, dýchá pomalu nebo nepravidelně, je studený, zvrací v bezvědomí, má křeče – ohrožení života.",
 "Nenechávej ho samotného.\n"
 "Spí: zotavovací poloha na boku, přikryj, kontroluj dech.\n"
 "Je-li při vědomí, ať pije vodu.",
 "Nedávej kávu, studenou sprchu, nevyvolávej zvracení.",
 "", "", "unconscious hypothermia"},

{"insect_sting", "Bodnutí hmyzem (včela, vosa, sršeň)", C::kPoison, U::kSelfCare,
 "bodnuti vosa vcela srsen bodla vosa zihadlo otok po bodnuti",
 "Dušnost, otok jazyka nebo krku, slabost, kopřivka po celém těle: anafylaxe. Bodnutí do úst nebo krku – otok může ucpat dýchací cesty.",
 "Včelí žihadlo vyndej co nejdřív seškrábnutím nehtem nebo kartou.\n"
 "Omyj a chlaď ledem přes látku.\n"
 "Svědění a otok: antihistaminikum (cetirizin), hydrokortizonový krém.\n"
 "Bodnutí v ústech: cucat led, dát antihistaminikum a sledovat dýchání.",
 "Nemačkej žihadlo prsty.",
 "cetirizine loratadine hydrocortisone",
 "Plantago major|Jitrocel větší|rozmačkaný list přiložit na bodnutí\n"
 "Allium cepa|Cibule kuchyňská|plátek přiložit na místo bodnutí",
 "anaphylaxis"},

{"tick", "Klíště", C::kPoison, U::kSelfCare,
 "kliste klíště prisate kliste vytahnout kliste borelioza encefalitida",
 "Červená skvrna, která se zvětšuje (a bledne uprostřed) za 3–30 dní, horečka, bolest kloubů, hlavy, ztuhlý krk po přisátí: k lékaři.",
 "Klíště odstraň co nejdřív – čím dřív, tím menší riziko boreliózy.\n"
 "Pinzetou nebo kartou na klíšťata chyť klíště těsně u kůže a pomalu vytáhni kolmo, nebo kývavým pohybem.\n"
 "Místo vydezinfikuj, umyj ruce.\n"
 "Zapiš si datum a místo. Měsíc sleduj kůži a zdraví.",
 "Nemaž klíště olejem, krémem ani lakem.\n"
 "Nemačkej jeho tělo.",
 "octenidine povidone_iodine", "", ""},

{"snake_bite", "Uštknutí hadem (zmije)", C::kPoison, U::kEmergency,
 "ustknuti had zmije kousl had uštknutí",
 "Otok šířící se po končetině, zvracení, průjem, slabost, dušnost, otok jazyka – co nejdřív k lékaři (protijed je jen v nemocnici). Nejvíc ohrožené jsou děti a staří lidé.",
 "Zachovej klid, polož ho, končetinu znehybni (dlaha, šátek) a drž ji níž než srdce.\n"
 "Sundej prstýnky, hodinky (bude otok).\n"
 "Ránu omyj, sterilně překryj.\n"
 "Zapiš čas uštknutí. Nenech ho chodit – nes nebo vez ho k lékaři.",
 "Nevysávej ránu, neřež do ní, nechlaď ledem.\n"
 "Nedávej škrtidlo.\n"
 "Nedávej alkohol.",
 "paracetamol", "", "anaphylaxis"},

{"jellyfish", "Medúza, mořští živočichové", C::kPoison, U::kSelfCare,
 "meduza medúza zahnal plave more ostny jezek",
 "Dušnost, bolest na hrudi, silná reakce, rozsáhlé zasažení, u dětí.",
 "Vyjdi z vody.\n"
 "Medúza: opláchni mořskou vodou (ne sladkou), zbytky chapadel seber pinzetou nebo kartou.\n"
 "Horká voda (snesitelná, asi 45 °C) 20 minut zmírní bolest.\n"
 "Ostny ježovky: vytáhni, horká voda.",
 "Netři místo pískem ani ručníkem.",
 "hydrocortisone paracetamol", "", "anaphylaxis"},

// ── Teplo, chlad, voda ──────────────────────────────────────────────────────
{"heatstroke", "Úpal, úžeh, přehřátí", C::kEnvironment, U::kEmergency,
 "upal úpal uzeh přehřátí horko teplo nevolnost na slunci",
 "Horká kůže, zmatenost, křeče, zvracení, bezvědomí, teplota přes 40 °C – úpal ohrožuje život.",
 "Přesuň ho do stínu nebo chladu, svleč přebytečné oblečení.\n"
 "Chlaď: vlhké chladné obklady, ovívání, chladná voda na kůži, studené obklady do podpaží a třísel.\n"
 "Je-li při vědomí, ať pije po doušcích (voda, rehydratační roztok).\n"
 "Bezvědomí: zotavovací poloha a chlaď dál.",
 "Nedávej léky na horečku – nepomohou.\n"
 "Nedávej alkohol.",
 "ors", "", "dehydration sunburn"},

{"hypothermia", "Podchlazení", C::kEnvironment, U::kEmergency,
 "podchlazeni podchlazení zima třes prochladl studený zmrzlý",
 "Ospalost, zmatenost, přestává se třást, pomalé dýchání – těžké podchlazení.",
 "Přesuň ho do tepla nebo do závětří, chraň před zemí (karimatka).\n"
 "Sundej mokré oblečení, zabal do suchých dek, čepice, alufólie.\n"
 "Je-li při vědomí: teplé sladké nápoje.\n"
 "Teplé (ne horké) láhve na hrudník, podpaží, třísla.\n"
 "Zacházej s ním šetrně, nenech ho chodit.",
 "Nemasíruj a netři končetiny.\n"
 "Nedávej alkohol.\n"
 "Nedávej horkou koupel.",
 "", "", "frostbite cpr"},

{"frostbite", "Omrzliny", C::kEnvironment, U::kDoctor,
 "omrzliny omrzly prsty bile prsty necitlive zmrzle",
 "Šedobílá, tvrdá, necitlivá kůže, puchýře: k lékaři.",
 "Přesuň se do tepla.\n"
 "Ohřívej v teplé vodě 37–39 °C (snesitelná pro loket) 20–30 minut.\n"
 "Pak volně, suše překryj, prsty prolož gázou.\n"
 "Bolest: ibuprofen.",
 "Netři sněhem, nemasíruj.\n"
 "Neohřívej, hrozí-li znovu zmrznutí.\n"
 "Neohřívej u ohně – necitlivá kůže se spálí.",
 "ibuprofen paracetamol", "", "hypothermia"},

{"lightning", "Zásah bleskem", C::kEnvironment, U::kEmergency,
 "blesk zasah bleskem bourka uder blesku",
 "Bezvědomí, zástava dechu, popáleniny po zásahu.",
 "Zasažený člověk není pod proudem – můžeš se ho dotknout.\n"
 "Nedýchá: resuscitace hned, má velkou šanci.\n"
 "Ošetři popáleniny.\n"
 "Sám se chraň: dřep se sraženýma nohama, ne pod osamělý strom.",
 "", "", "", "cpr burns"},

{"childbirth", "Náhlý porod", C::kEmergency, U::kEmergency,
 "porod rodi plodova voda tlaci dite se rodi miminko",
 "Silné krvácení po porodu, dítě nedýchá, pupečník venku jako první, dítě nožkama napřed.",
  "Rodička ať leží nebo polosedí, pod ní čisté ručníky.\n"
 "Dítě netahej, jen podpírej hlavičku, jak vychází.\n"
 "Po narození dítě osuš, polož ho matce na břicho kůží na kůži, přikryj oba.\n"
 "Pupečník nestříhej hned. Až přestane pulzovat (asi 10 minut), podvaž ho čistou tkaničkou ve dvou místech 10 a 15 cm od bříška a přestřihni mezi nimi čistými nůžkami.\n"
 "Placenta vyjde sama do 30–60 minut – netahej. Pak masíruj dělohu přes břicho a přilož dítě k prsu: sníží to krvácení.\n"
 "Dítě nedýchá: otři ho, mnuj záda, pak 5 vdechů a resuscitace.",
 "Netahej za dítě ani za pupečník.",
 "", "", "cpr_child"},
};

}  // namespace

const char* const kPoisoningSteps =
    "Vypláchni ústa vodou a zbytky vyplivni.\n"
    "Do hodiny od snědení pomůže živočišné uhlí: rozdrť tablety ve vodě, dospělý až 50 g, dítě 1 g na kg váhy.\n"
    "Nevyvolávej zvracení a nedávej mléko ani olej.\n"
    "Zapiš, co, kolik a kdy snědl; schovej zbytky rostliny nebo houby.\n"
    "Kdo jedl totéž, hlídej i jeho. Sleduj dech a vědomí, při bezvědomí zotavovací poloha.\n"
    "U hub mohou příznaky přijít až za 6–24 hodin a pak se na čas uleví – i tak co nejdřív k lékaři.";

// The order in which to deal with an injured person when no help is coming.
const char* const kPriorities =
    "1. Bezpečí – tvoje i jeho.\n"
    "2. Masivní krvácení – zastav ho hned (tlak, škrtidlo).\n"
    "3. Dýchací cesty – zaklonit hlavu, vyčistit ústa, dušení.\n"
    "4. Dýchání – nedýchá: resuscitace.\n"
    "5. Oběh a šok – položit, zahřát, zastavit krvácení.\n"
    "6. Podchlazení – izolace od země, suché oblečení, přikrýt.\n"
    "7. Pak zbytek: hlava, zlomeniny, rány, bolest.";

size_t TopicCount() { return sizeof(kTopics) / sizeof(kTopics[0]); }

const Topic& TopicAt(size_t index) { return kTopics[index]; }

const Topic* FindTopic(const char* id) {
  if (id == nullptr) {
    return nullptr;
  }
  for (const Topic& topic : kTopics) {
    if (std::strcmp(topic.id, id) == 0) {
      return &topic;
    }
  }
  return nullptr;
}

const char* CategoryName(Category category) {
  switch (category) {
    case Category::kEmergency: return "Ohrožení života";
    case Category::kInjury: return "Úrazy a rány";
    case Category::kBandage: return "Obvazy a dlahy";
    case Category::kIllness: return "Nemoci a příznaky";
    case Category::kPoison: return "Otravy a bodnutí";
    case Category::kEnvironment: return "Teplo, chlad, voda";
    case Category::kCount: break;
  }
  return "";
}

}  // namespace lilygo_box::firstaid
