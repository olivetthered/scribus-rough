/*
For general Scribus (>=1.3.2) copyright and licensing information please refer
to the COPYING file provided with the program. Following this notice may exist
a copyright and/or license notice that predates the release of Scribus 1.3.2
for which a new license (GPL+exception) is in place.
*/

#include "slaoutput.h"

#include <poppler/GlobalParams.h>
#include <poppler/poppler-config.h>
#include <poppler/FileSpec.h>
#include <poppler/fofi/FoFiTrueType.h>
#include <QApplication>
#include <QFile>
#include "commonstrings.h"
#include "loadsaveplugin.h"
#include "sccolorengine.h"
#include "util.h"
#include "util_math.h"
#include <tiffio.h>
#include "../../../../../QT/5.14.2/msvc2017_64/include/QtGui/qfontdatabase.h"

namespace
{
	// Compute the intersection of two paths while considering the fillrule of each of them.
	// QPainterPath has the right interface to do the operation but is currently buggy.
	// See for example https://bugreports.qt.io/browse/QTBUG-83102. Thus this function
	// applies some heuristics to find the best result. As soon QPainterPath is fixed
	// one can just use a.intersected(b) wherever this function is called.
	// TODO: Find an alternative to QPainterPath that works for different fill rules.
	QPainterPath intersection(QPainterPath const &a, QPainterPath const &b)
	{
		// An empty path is treated like the whole area.
		if (a.elementCount() == 0)
			return b;
		if (b.elementCount() == 0)
			return a;

		QPainterPath ret_a = a.intersected(b);
		QPainterPath ret_b = b.intersected(a);
		// Sometimes the resulting paths are not closed even though they should.
		// Close them now.
		ret_a.closeSubpath();
		ret_b.closeSubpath();

		// Most of the time one of the two operations returns an empty path while the other
		// gives us the desired result. Return the non-empty one.
		if (ret_a.elementCount() == 0)
			return ret_b;
		if (ret_b.elementCount() == 0)
			return ret_a;

		// There are cases where both intersections are not empty but one of them is quite
		// complicated with several subpaths, etc. We return the simpler one.
		return (ret_a.elementCount() <= ret_b.elementCount()) ? ret_a : ret_b;
	}

	// Invert preblending matte values into the color values. Assuming that c and alpha are RGBA components
	// between 0 and 255.
	int unblendMatte(int c, int alpha, int matte)
	{
		if (alpha == 0)
			return matte;
		int ret = matte + ((c - matte) * 255) / alpha;
		if (ret < 0)
			return 0;
		if (ret > 255)
			return 255;
		return ret;
	}
}

LinkSubmitForm::LinkSubmitForm(Object *actionObj)
{
	if (!actionObj->isDict())
		return;

	Object obj1 = actionObj->dictLookup("F");
	if (!obj1.isNull())
	{
		if (obj1.isDict())
		{
			Object obj3 = obj1.dictLookup("FS");
			if (!obj3.isNull())
			{
				if (obj3.isName())
				{
					POPPLER_CONST char *name = obj3.getName();
					if (!strcmp(name, "URL"))
					{
						Object obj2 = obj1.dictLookup("F");
						if (!obj2.isNull())
							fileName = obj2.getString()->copy();
					}
				}
			}
		}
	}
	obj1 = actionObj->dictLookup("Flags");
	if (!obj1.isNull())
	{
		if (obj1.isNum())
			m_flags = obj1.getInt();
	}
}

LinkSubmitForm::~LinkSubmitForm()
{
	delete fileName;
}

LinkImportData::LinkImportData(Object *actionObj)
{
	if (!actionObj->isDict())
		return;
	Object obj1 = actionObj->dictLookup("F");
	if (obj1.isNull())
		return;

	Object obj3 = getFileSpecNameForPlatform(&obj1);
	if (!obj3.isNull())
		fileName = obj3.getString()->copy();
}

LinkImportData::~LinkImportData()
{
	delete fileName;
}

AnoOutputDev::~AnoOutputDev()
{
	delete m_fontName;
	delete m_itemText;
}

AnoOutputDev::AnoOutputDev(ScribusDoc* doc, QStringList *importedColors)
{
	m_doc = doc;
	m_importedColors = importedColors;
	CurrColorText = "Black";
	CurrColorFill = CommonStrings::None;
	CurrColorStroke = CommonStrings::None;
}

void AnoOutputDev::eoFill(GfxState *state)
{
	int shade = 100;
	CurrColorFill = getColor(state->getFillColorSpace(), state->getFillColor(), &shade);
}

void AnoOutputDev::fill(GfxState *state)
{
	int shade = 100;
	CurrColorFill = getColor(state->getFillColorSpace(), state->getFillColor(), &shade);
}

void AnoOutputDev::stroke(GfxState *state)
{
	int shade = 100;
	CurrColorStroke = getColor(state->getStrokeColorSpace(), state->getStrokeColor(), &shade);
}

void AnoOutputDev::drawString(GfxState *state, POPPLER_CONST GooString *s)
{
	int shade = 100;
	CurrColorText = getColor(state->getFillColorSpace(), state->getFillColor(), &shade);
	m_fontSize = state->getFontSize();
	if (state->getFont())
		m_fontName = state->getFont()->getName()->copy();
	m_itemText = s->copy();
}

QString AnoOutputDev::getColor(GfxColorSpace *color_space, POPPLER_CONST_070 GfxColor *color, int *shade)
{
	QString fNam;
	QString namPrefix = "FromPDF";
	ScColor tmp;
	tmp.setSpotColor(false);
	tmp.setRegistrationColor(false);
	*shade = 100;
	if ((color_space->getMode() == csDeviceRGB) || (color_space->getMode() == csCalRGB))
	{
		GfxRGB rgb;
		color_space->getRGB(color, &rgb);
		double Rc = colToDbl(rgb.r);
		double Gc = colToDbl(rgb.g);
		double Bc = colToDbl(rgb.b);
		tmp.setRgbColorF(Rc, Gc, Bc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	else if (color_space->getMode() == csDeviceCMYK)
	{
		GfxCMYK cmyk;
		color_space->getCMYK(color, &cmyk);
		double Cc = colToDbl(cmyk.c);
		double Mc = colToDbl(cmyk.m);
		double Yc = colToDbl(cmyk.y);
		double Kc = colToDbl(cmyk.k);
		tmp.setCmykColorF(Cc, Mc, Yc, Kc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	else if ((color_space->getMode() == csCalGray) || (color_space->getMode() == csDeviceGray))
	{
		GfxGray gray;
		color_space->getGray(color, &gray);
		double Kc = 1.0 - colToDbl(gray);
		tmp.setCmykColorF(0, 0, 0, Kc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	else if (color_space->getMode() == csSeparation)
	{
		GfxSeparationColorSpace* sepColorSpace = (GfxSeparationColorSpace*)color_space;
		GfxColorSpace* altColorSpace = sepColorSpace->getAlt();
		QString name = QString(sepColorSpace->getName()->getCString());
		bool isRegistrationColor = (name == "All");
		if (isRegistrationColor)
		{
			tmp.setCmykColorF(1.0, 1.0, 1.0, 1.0);
			tmp.setRegistrationColor(true);
			name = "Registration";
		}
		else if ((altColorSpace->getMode() == csDeviceRGB) || (altColorSpace->getMode() == csCalRGB))
		{
			double x = 1.0;
			double comps[gfxColorMaxComps];
			sepColorSpace->getFunc()->transform(&x, comps);
			tmp.setRgbColorF(comps[0], comps[1], comps[2]);
		}
		else if ((altColorSpace->getMode() == csCalGray) || (altColorSpace->getMode() == csDeviceGray))
		{
			double x = 1.0;
			double comps[gfxColorMaxComps];
			sepColorSpace->getFunc()->transform(&x, comps);
			tmp.setCmykColorF(0.0, 0.0, 0.0, 1.0 - comps[0]);
		}
		else if (altColorSpace->getMode() == csLab)
		{
			double x = 1.0;
			double comps[gfxColorMaxComps];
			sepColorSpace->getFunc()->transform(&x, comps);
			tmp.setLabColor(comps[0], comps[1], comps[2]);
		}
		else
		{
			GfxCMYK cmyk;
			color_space->getCMYK(color, &cmyk);
			double Cc = colToDbl(cmyk.c);
			double Mc = colToDbl(cmyk.m);
			double Yc = colToDbl(cmyk.y);
			double Kc = colToDbl(cmyk.k);
			tmp.setCmykColorF(Cc, Mc, Yc, Kc);
		}
		tmp.setSpotColor(true);

		fNam = m_doc->PageColors.tryAddColor(name, tmp);
		*shade = qRound(colToDbl(color->c[0]) * 100);
	}
	else
	{
		GfxRGB rgb;
		color_space->getRGB(color, &rgb);
		double Rc = colToDbl(rgb.r);
		double Gc = colToDbl(rgb.g);
		double Bc = colToDbl(rgb.b);
		tmp.setRgbColorF(Rc, Gc, Bc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	//	qDebug() << "update fill color other colorspace" << color_space->getMode() << "treating as rgb" << Rc << Gc << Bc;
	}
	if (fNam == namPrefix+tmp.name())
		m_importedColors->append(fNam);
	return fNam;
}

SlaOutputDev::SlaOutputDev(ScribusDoc* doc, QList<PageItem*> *Elements, QStringList *importedColors, int flags)
{
	m_doc = doc;
	m_Elements = Elements;
	pushGroup();
	m_importedColors = importedColors;
	CurrColorStroke = "Black";
	CurrColorFill = "Black";
	tmpSel = new Selection(m_doc, false);
	importerFlags = flags;
	currentLayer = m_doc->activeLayer();
	layersSetByOCG = false;

	//TODO: from the ui, this needs to come from whereever the UI parameters are set.	

	//text handling
	_in_text_object = false;
	_invalidated_style = true;
	_need_font_update = true;		
	_font_scaling = 1.0;
	//_current_font = NULL;

	QFontDatabase fontDatabase;

	// Fill _availableFontNames
	//TODO: writing system needs to be sup[ported properly
	QStringList _availableFontNames = fontDatabase.families();

}

SlaOutputDev::~SlaOutputDev()
{
	m_groupStack.clear();
	tmpSel->clear();
	delete tmpSel;
	delete m_fontEngine;
}

/* get Actions not implemented by Poppler */
LinkAction* SlaOutputDev::SC_getAction(AnnotWidget *ano)
{
	LinkAction *linkAction = nullptr;
	Object obj;
	Ref refa = ano->getRef();

	obj = xref->fetch(refa.num, refa.gen);
	if (obj.isDict())
	{
		Dict* adic = obj.getDict();
		POPPLER_CONST_075 Object POPPLER_REF additionalActions = adic->lookupNF("A");
		Object additionalActionsObject = additionalActions.fetch(pdfDoc->getXRef());
		if (additionalActionsObject.isDict())
		{
			Object actionObject = additionalActionsObject.dictLookup("S");
			if (actionObject.isName("ImportData"))
			{
				linkAction = new LinkImportData(&additionalActionsObject);
			}
			else if (actionObject.isName("SubmitForm"))
			{
				linkAction = new LinkSubmitForm(&additionalActionsObject);
			}
		}
	}
	return linkAction;
}

/* Replacement for the crippled Poppler function LinkAction* AnnotWidget::getAdditionalAction(AdditionalActionsType type) */
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
std::unique_ptr<LinkAction> SlaOutputDev::SC_getAdditionalAction(const char *key, AnnotWidget *ano)
{
	std::unique_ptr<LinkAction> linkAction;
#else
LinkAction* SlaOutputDev::SC_getAdditionalAction(const char *key, AnnotWidget *ano)
{
	LinkAction *linkAction = nullptr;
#endif
	Object obj;
	Ref refa = ano->getRef();

	obj = xref->fetch(refa.num, refa.gen);
	if (obj.isDict())
	{
		Dict* adic = obj.getDict();
		POPPLER_CONST_075 Object POPPLER_REF additionalActions = adic->lookupNF("AA");
		Object additionalActionsObject = additionalActions.fetch(pdfDoc->getXRef());
		if (additionalActionsObject.isDict())
		{
			Object actionObject = additionalActionsObject.dictLookup(key);
			if (actionObject.isDict())
				linkAction = LinkAction::parseAction(&actionObject, pdfDoc->getCatalog()->getBaseURI());
		}
	}
	return linkAction;
}

GBool SlaOutputDev::annotations_callback(Annot *annota, void *user_data)
{
	SlaOutputDev *dev = (SlaOutputDev*)user_data;
	PDFRectangle *box = annota->getRect();
	double xCoor = dev->m_doc->currentPage()->xOffset() + box->x1 - dev->cropOffsetX;
	double yCoor = dev->m_doc->currentPage()->yOffset() + dev->m_doc->currentPage()->height() - box->y2 + dev->cropOffsetY;
	double width = box->x2 - box->x1;
	double height = box->y2 - box->y1;
	if (dev->rotate == 90)
	{
		xCoor = dev->m_doc->currentPage()->xOffset() - dev->cropOffsetX + box->y2;
		yCoor = dev->m_doc->currentPage()->yOffset() + dev->cropOffsetY + box->x1;
	}
	else if (dev->rotate == 180)
	{
		xCoor = dev->m_doc->currentPage()->xOffset() - dev->cropOffsetX + dev->m_doc->currentPage()->width() - box->x1;
		yCoor = dev->m_doc->currentPage()->yOffset() + dev->cropOffsetY + box->y2;
	}
	else if (dev->rotate == 270)
	{
		xCoor = dev->m_doc->currentPage()->xOffset() - dev->cropOffsetX + dev->m_doc->currentPage()->width() - box->y2;
		yCoor = dev->m_doc->currentPage()->yOffset() + dev->cropOffsetY + dev->m_doc->currentPage()->height() - box->x1;
	}
	bool retVal = true;
	if (annota->getType() == Annot::typeText)
		retVal = !dev->handleTextAnnot(annota, xCoor, yCoor, width, height);
	else if (annota->getType() == Annot::typeLink)
		retVal = !dev->handleLinkAnnot(annota, xCoor, yCoor, width, height);
	else if (annota->getType() == Annot::typeWidget)
		retVal = !dev->handleWidgetAnnot(annota, xCoor, yCoor, width, height);
	return retVal;
}

bool SlaOutputDev::handleTextAnnot(Annot* annota, double xCoor, double yCoor, double width, double height)
{
	AnnotText *anl = (AnnotText*)annota;
	int z = m_doc->itemAdd(PageItem::TextFrame, PageItem::Rectangle, xCoor, yCoor, width, height, 0, CommonStrings::None, CommonStrings::None);
	PageItem *ite = m_doc->Items->at(z);
	int flg = annota->getFlags();
	if (!(flg & 16))
		ite->setRotation(rotate, true);
	ite->ClipEdited = true;
	ite->FrameType = 3;
	ite->setFillEvenOdd(false);
	ite->Clip = flattenPath(ite->PoLine, ite->Segments);
	ite->ContourLine = ite->PoLine.copy();
	ite->setTextFlowMode(PageItem::TextFlowDisabled);
	m_Elements->append(ite);
	if (m_groupStack.count() != 0)
	{
		m_groupStack.top().Items.append(ite);
		applyMask(ite);
	}
	ite->setIsAnnotation(true);
	ite->AutoName = false;
	ite->annotation().setType(Annotation::Text);
	ite->annotation().setActionType(Annotation::Action_None);
	ite->annotation().setAnOpen(anl->getOpen());
	QString iconName = UnicodeParsedString(anl->getIcon());
	if (iconName == "Note")
		ite->annotation().setIcon(Annotation::Icon_Note);
	else if (iconName == "Comment")
		ite->annotation().setIcon(Annotation::Icon_Comment);
	else if (iconName == "Key")
		ite->annotation().setIcon(Annotation::Icon_Key);
	else if (iconName == "Help")
		ite->annotation().setIcon(Annotation::Icon_Help);
	else if (iconName == "NewParagraph")
		ite->annotation().setIcon(Annotation::Icon_NewParagraph);
	else if (iconName == "Paragraph")
		ite->annotation().setIcon(Annotation::Icon_Paragraph);
	else if (iconName == "Insert")
		ite->annotation().setIcon(Annotation::Icon_Insert);
	else if (iconName == "Cross")
		ite->annotation().setIcon(Annotation::Icon_Cross);
	else if (iconName == "Circle")
		ite->annotation().setIcon(Annotation::Icon_Circle);
	else
		ite->annotation().setIcon(Annotation::Icon_Note);
	ite->setItemName( CommonStrings::itemName_TextAnnotation + QString("%1").arg(m_doc->TotalItems));
	ite->itemText.insertChars(UnicodeParsedString(annota->getContents()));
	ite->itemText.trim();
	return true;
}

bool SlaOutputDev::handleLinkAnnot(Annot* annota, double xCoor, double yCoor, double width, double height)
{
	AnnotLink *anl = (AnnotLink*)annota;
	LinkAction *act = anl->getAction();
	if (!act)
		return false;
	bool validLink = false;
	int pagNum = 0;
	int xco = 0;
	int yco = 0;
	QString fileName = "";
	if (act->getKind() == actionGoTo)
	{
		LinkGoTo *gto = (LinkGoTo*) act;
		POPPLER_CONST LinkDest *dst = gto->getDest();
		if (dst)
		{
			if (dst->getKind() == destXYZ)
			{
				if (dst->isPageRef())
				{
					Ref dstr = dst->getPageRef();
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 76, 0)
					pagNum = pdfDoc->findPage(dstr);
#else
					pagNum = pdfDoc->findPage(dstr.num, dstr.gen);
#endif
				}
				else
					pagNum = dst->getPageNum();
				xco = dst->getLeft();
				yco = dst->getTop();
				validLink = true;
			}
		}
		else
		{
			POPPLER_CONST GooString *ndst = gto->getNamedDest();
			if (ndst)
			{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
				std::unique_ptr<LinkDest> dstn = pdfDoc->findDest(ndst);
#else
				LinkDest *dstn = pdfDoc->findDest(ndst);
#endif
				if (dstn)
				{
					if (dstn->getKind() == destXYZ)
					{
						if (dstn->isPageRef())
						{
							Ref dstr = dstn->getPageRef();
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 76, 0)
							pagNum = pdfDoc->findPage(dstr);
#else
							pagNum = pdfDoc->findPage(dstr.num, dstr.gen);
#endif
						}
						else
							pagNum = dstn->getPageNum();
						xco = dstn->getLeft();
						yco = dstn->getTop();
						validLink = true;
					}
				}
			}
		}
	}
	else if (act->getKind() == actionGoToR)
	{
		LinkGoToR *gto = (LinkGoToR*)act;
		fileName = UnicodeParsedString(gto->getFileName());
		POPPLER_CONST LinkDest *dst = gto->getDest();
		if (dst)
		{
			if (dst->getKind() == destXYZ)
			{
				pagNum = dst->getPageNum();
				xco = dst->getLeft();
				yco = dst->getTop();
				validLink = true;
			}
		}
		else
		{
			POPPLER_CONST GooString *ndst = gto->getNamedDest();
			if (ndst)
			{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
				std::unique_ptr<LinkDest> dstn = pdfDoc->findDest(ndst);
#else
				LinkDest *dstn = pdfDoc->findDest(ndst);
#endif
				if (dstn)
				{
					if (dstn->getKind() == destXYZ)
					{
						pagNum = dstn->getPageNum();
						xco = dstn->getLeft();
						yco = dstn->getTop();
						validLink = true;
					}
				}
			}
		}
	}
	else if (act->getKind() == actionURI)
	{
		LinkURI *gto = (LinkURI*)act;
		validLink = true;
		fileName = UnicodeParsedString(gto->getURI());
	}
	if (validLink)
	{
		int z = m_doc->itemAdd(PageItem::TextFrame, PageItem::Rectangle, xCoor, yCoor, width, height, 0, CommonStrings::None, CommonStrings::None);
		PageItem *ite = m_doc->Items->at(z);
		int flg = annota->getFlags();
		if (!(flg & 16))
			ite->setRotation(rotate, true);
		ite->ClipEdited = true;
		ite->FrameType = 3;
		ite->setFillEvenOdd(false);
		ite->Clip = flattenPath(ite->PoLine, ite->Segments);
		ite->ContourLine = ite->PoLine.copy();
		ite->setTextFlowMode(PageItem::TextFlowDisabled);
		m_Elements->append(ite);
		if (m_groupStack.count() != 0)
		{
			m_groupStack.top().Items.append(ite);
			applyMask(ite);
		}
		ite->setIsAnnotation(true);
		ite->AutoName = false;
		if (act->getKind() == actionGoTo)
		{
			ite->annotation().setZiel((pagNum > 0) ? (pagNum - 1) : (m_actPage - 1));
			ite->annotation().setAction(QString("%1 %2").arg(xco).arg(yco));
			ite->annotation().setActionType(2);
		}
		else if (act->getKind() == actionGoToR)
		{
			ite->annotation().setZiel((pagNum > 0) ? (pagNum - 1) : (m_actPage - 1));
			ite->annotation().setExtern(fileName);
			ite->annotation().setAction(QString("%1 %2").arg(xco).arg(yco));
			ite->annotation().setActionType(9);
		}
		else if (act->getKind() == actionURI)
		{
			ite->annotation().setAction("");
			ite->annotation().setExtern(fileName);
			ite->annotation().setActionType(8);
		}
		ite->annotation().setType(Annotation::Link);
		ite->setItemName( CommonStrings::itemName_LinkAnnotation + QString("%1").arg(m_doc->TotalItems));
	}
	return validLink;
}

bool SlaOutputDev::handleWidgetAnnot(Annot* annota, double xCoor, double yCoor, double width, double height)
{
	bool retVal = false;
	bool found = false;

	if (!m_formWidgets)
		return false;

	int formcount = m_formWidgets->getNumWidgets();
	for (int i = 0; i < formcount; ++i)
	{
		FormWidget *fm = m_formWidgets->getWidget(i);
		if (!fm)
			continue;
		AnnotWidget *ano = fm->getWidgetAnnotation();
		if (!ano)
			continue;
		if (ano != (AnnotWidget*) annota)
			continue;
		found = true;
		int wtyp = -1;
		if (fm->getType() == formButton)
		{
			FormWidgetButton *btn = (FormWidgetButton*)fm;
			if (btn)
			{
				if (btn->getButtonType() == formButtonCheck)
				{
					wtyp = Annotation::Checkbox;
					retVal = true;
				}
				else if (btn->getButtonType() == formButtonPush)
				{
					wtyp = Annotation::Button;
					retVal = true;
				}
				else if (btn->getButtonType() == formButtonRadio)
				{
					wtyp = Annotation::RadioButton;
					retVal = true;
				}
			}
		}
		else if (fm->getType() == formText)
		{
			wtyp = Annotation::Textfield;
			retVal = true;
		}
		else if (fm->getType() == formChoice)
		{
			FormWidgetChoice *btn = (FormWidgetChoice*)fm;
			if (btn)
			{
				if (btn->isCombo())
				{
					wtyp = Annotation::Combobox;
					retVal = true;
				}
				else if (btn->isListBox())
				{
					wtyp = Annotation::Listbox;
					retVal = true;
				}
			}
		}
		if (retVal)
		{
			AnnotAppearanceCharacs *achar = ano->getAppearCharacs();
			bool fgFound = false;
			bool bgFound = false;
			if (achar)
			{
				POPPLER_CONST AnnotColor *bgCol = achar->getBackColor();
				if (bgCol)
				{
					bgFound = true;
					CurrColorFill = getAnnotationColor(bgCol);
				}
				else
					CurrColorFill = CommonStrings::None;
				POPPLER_CONST AnnotColor *fgCol = achar->getBorderColor();
				if (fgCol)
				{
					fgFound = true;
					CurrColorStroke = getAnnotationColor(fgCol);
				}
				else
				{
					fgCol = achar->getBackColor();
					if (fgCol)
						CurrColorStroke = getAnnotationColor(fgCol);
					else
						CurrColorStroke = CommonStrings::None;
				}
			}
			QString CurrColorText = "Black";
			double fontSize = 12;
			QString fontName = "";
			QString itemText = "";
			AnnotAppearance *apa = annota->getAppearStreams();
			if (apa || !achar)
			{
				AnoOutputDev *Adev = new AnoOutputDev(m_doc, m_importedColors);
				Gfx *gfx = new Gfx(pdfDoc, Adev, pdfDoc->getPage(m_actPage)->getResourceDict(), annota->getRect(), nullptr);
				ano->draw(gfx, false);
				if (!bgFound)
					CurrColorFill = Adev->CurrColorFill;
				if (!fgFound)
					CurrColorStroke = Adev->CurrColorStroke;
				CurrColorText = Adev->CurrColorText;
				fontSize = Adev->m_fontSize;
				fontName = UnicodeParsedString(Adev->m_fontName);
				itemText = UnicodeParsedString(Adev->m_itemText);
				delete gfx;
				delete Adev;
			}
			int z = m_doc->itemAdd(PageItem::TextFrame, PageItem::Rectangle, xCoor, yCoor, width, height, 0, CurrColorFill, CommonStrings::None);
			PageItem *ite = m_doc->Items->at(z);
			int flg = annota->getFlags();
			if (!(flg & 16))
				ite->setRotation(rotate, true);
			ite->ClipEdited = true;
			ite->FrameType = 3;
			ite->setFillEvenOdd(false);
			ite->Clip = flattenPath(ite->PoLine, ite->Segments);
			ite->ContourLine = ite->PoLine.copy();
			ite->setTextFlowMode(PageItem::TextFlowDisabled);
			m_Elements->append(ite);
			if (m_groupStack.count() != 0)
			{
				m_groupStack.top().Items.append(ite);
				applyMask(ite);
			}
			ite->setIsAnnotation(true);
			ite->AutoName = false;
			AnnotBorder *brd = annota->getBorder();
			if (brd)
			{
				int bsty = brd->getStyle();
				if (bsty == AnnotBorder::borderDashed)
					bsty = 1;
				else if (bsty == AnnotBorder::borderBeveled)
					bsty = 3;
				else if (bsty == AnnotBorder::borderInset)
					bsty = 4;
				else if (bsty == AnnotBorder::borderUnderlined)
					bsty = 2;
				ite->annotation().setBorderStyle(bsty);
				ite->annotation().setBorderColor(CurrColorStroke);
				ite->annotation().setBorderWidth(qRound(brd->getWidth()));
			}
			else
			{
				ite->annotation().setBorderStyle(0);
				ite->annotation().setBorderColor(CommonStrings::None);
				ite->annotation().setBorderWidth(0);
			}
			QString tmTxt = "";
			tmTxt = UnicodeParsedString(fm->getPartialName());
			if (!tmTxt.isEmpty())
				ite->setItemName(tmTxt);
			tmTxt = "";
			tmTxt = UnicodeParsedString(fm->getAlternateUiName());
			if (!tmTxt.isEmpty())
				ite->annotation().setToolTip(tmTxt);
			tmTxt = "";
			if (achar)
			{
				tmTxt = UnicodeParsedString(achar->getRolloverCaption());
				if (!tmTxt.isEmpty())
					ite->annotation().setRollOver(tmTxt);
				tmTxt = "";
				tmTxt = UnicodeParsedString(achar->getAlternateCaption());
				if (!tmTxt.isEmpty())
					ite->annotation().setDown(tmTxt);
			}
			ite->annotation().setType(wtyp);
			ite->annotation().setFlag(0);
			if (flg & 2)
				ite->annotation().setVis(1);
			if (flg & 32)
				ite->annotation().setVis(3);
			if (wtyp == Annotation::Button)
			{
				ite->setFillColor(CurrColorFill);
				if (achar)
					ite->itemText.insertChars(UnicodeParsedString(achar->getNormalCaption()));
				else
					ite->itemText.insertChars(itemText);
				applyTextStyle(ite, fontName, CurrColorText, fontSize);
				ite->annotation().addToFlag(Annotation::Flag_PushButton);
				FormWidgetButton *btn = (FormWidgetButton*)fm;
				if (!btn->isReadOnly())
					ite->annotation().addToFlag(Annotation::Flag_Edit);
				handleActions(ite, ano);
			}
			else if (wtyp == Annotation::Textfield)
			{
				FormWidgetText *btn = (FormWidgetText*)fm;
				if (btn)
				{
					ite->itemText.insertChars(UnicodeParsedString(btn->getContent()));
					applyTextStyle(ite, fontName, CurrColorText, fontSize);
					ite->itemText.trim();
					if (btn->isMultiline())
						ite->annotation().addToFlag(Annotation::Flag_Multiline);
					if (btn->isPassword())
						ite->annotation().addToFlag(Annotation::Flag_Password);
					if (btn->noSpellCheck())
						ite->annotation().addToFlag(Annotation::Flag_DoNotSpellCheck);
					if (btn->noScroll())
						ite->annotation().addToFlag(Annotation::Flag_DoNotScroll);
					int mxLen = btn->getMaxLen();
					if (mxLen > 0)
						ite->annotation().setMaxChar(mxLen);
					else
						ite->annotation().setMaxChar(-1);
					if (!btn->isReadOnly())
						ite->annotation().addToFlag(Annotation::Flag_Edit);
					handleActions(ite, ano);
				}
			}
			else if (wtyp == Annotation::Checkbox)
			{
				FormWidgetButton *btn = (FormWidgetButton*)fm;
				if (btn)
				{
					ite->annotation().setIsChk(btn->getState());
					ite->annotation().setCheckState(ite->annotation().IsChk());
					handleActions(ite, ano);
					if (itemText == "4")
						ite->annotation().setChkStil(0);
					else if (itemText == "5")
						ite->annotation().setChkStil(1);
					else if (itemText == "F")
						ite->annotation().setChkStil(2);
					else if (itemText == "l")
						ite->annotation().setChkStil(3);
					else if (itemText == "H")
						ite->annotation().setChkStil(4);
					else if (itemText == "n")
						ite->annotation().setChkStil(5);
					else
						ite->annotation().setChkStil(0);
					if (!btn->isReadOnly())
						ite->annotation().addToFlag(Annotation::Flag_Edit);
				}
			}
			else if ((wtyp == Annotation::Combobox) || (wtyp == Annotation::Listbox))
			{
				FormWidgetChoice *btn = (FormWidgetChoice*)fm;
				if (btn)
				{
					if (wtyp == 5)
						ite->annotation().addToFlag(Annotation::Flag_Combo);
					int co = btn->getNumChoices();
					if (co > 0)
					{
						QString inh = UnicodeParsedString(btn->getChoice(0));
						for (int a = 1; a < co; a++)
						{
							inh += "\n" + UnicodeParsedString(btn->getChoice(a));
						}
						ite->itemText.insertChars(inh);
					}
					applyTextStyle(ite, fontName, CurrColorText, fontSize);
					if (!btn->isReadOnly())
						ite->annotation().addToFlag(Annotation::Flag_Edit);
					handleActions(ite, ano);
				}
			}
			else if (wtyp == Annotation::RadioButton)
			{
				FormWidgetButton *btn = (FormWidgetButton*)fm;
				if (btn)
				{
					ite->setItemName( CommonStrings::itemName_RadioButton + QString("%1").arg(m_doc->TotalItems));
					ite->annotation().setIsChk(btn->getState());
					ite->annotation().setCheckState(ite->annotation().IsChk());
					handleActions(ite, ano);
					m_radioButtons.insert(annota->getRef().num, ite);
				}
			}
		}
		break;
	}
	if (!found)
	{
		Object obj1;
		Ref refa = annota->getRef();
		obj1 = xref->fetch(refa.num, refa.gen);
		if (obj1.isDict())
		{
			Dict* dict = obj1.getDict();
			Object obj2 = dict->lookup("Kids");
			//childs
			if (obj2.isArray())
			{
				// Load children
				QList<int> radList;
				for (int i = 0; i < obj2.arrayGetLength(); i++)
				{
					POPPLER_CONST_075 Object POPPLER_REF childRef = obj2.arrayGetNF(i);
					if (!childRef.isRef())
						continue;
					Object childObj = obj2.arrayGet(i);
					if (!childObj.isDict())
						continue;
					const Ref ref = childRef.getRef();
					radList.append(ref.num);
				}
				QString tmTxt = UnicodeParsedString(annota->getName());
				m_radioMap.insert(tmTxt, radList);
			}
		}
	}
	return retVal;
}

void SlaOutputDev::applyTextStyle(PageItem* ite, const QString& fontName, const QString& textColor, double fontSize)
{
	CharStyle newStyle;
	newStyle.setFillColor(textColor);
	newStyle.setFontSize(fontSize * 10);
	if (!fontName.isEmpty())
	{
		SCFontsIterator it(*m_doc->AllFonts);
		for ( ; it.hasNext() ; it.next())
		{
			ScFace& face(it.current());
			if ((face.psName() == fontName) && (face.usable()) && (face.type() == ScFace::TTF))
			{
				newStyle.setFont(face);
				break;
			}
			if ((face.family() == fontName) && (face.usable()) && (face.type() == ScFace::TTF))
			{
				newStyle.setFont(face);
				break;
			}
			if ((face.scName() == fontName) && (face.usable()) && (face.type() == ScFace::TTF))
			{
				newStyle.setFont(face);
				break;
			}
		}
	}
	ParagraphStyle dstyle(ite->itemText.defaultStyle());
	dstyle.charStyle().applyCharStyle(newStyle);
	ite->itemText.setDefaultStyle(dstyle);
	ite->itemText.applyCharStyle(0, ite->itemText.length(), newStyle);
	ite->invalid = true;
}

void SlaOutputDev::handleActions(PageItem* ite, AnnotWidget *ano)
{
	LinkAction *Lact = ano->getAction();
	if (Lact)
	{
		if (Lact->getKind() == actionJavaScript)
		{
			LinkJavaScript *jsa = (LinkJavaScript*)Lact;
			if (jsa->isOk())
			{
				ite->annotation().setActionType(1);
				ite->annotation().setAction(UnicodeParsedString(jsa->getScript()));
			}
		}
		else if (Lact->getKind() == actionGoTo)
		{
			int pagNum = 0;
			int xco = 0;
			int yco = 0;
			LinkGoTo *gto = (LinkGoTo*)Lact;
			POPPLER_CONST LinkDest *dst = gto->getDest();
			if (dst)
			{
				if (dst->getKind() == destXYZ)
				{
					if (dst->isPageRef())
					{
						Ref dstr = dst->getPageRef();
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 76, 0)
						pagNum = pdfDoc->findPage(dstr);
#else
						pagNum = pdfDoc->findPage(dstr.num, dstr.gen);
#endif
					}
					else
						pagNum = dst->getPageNum();
					xco = dst->getLeft();
					yco = dst->getTop();
					ite->annotation().setZiel((pagNum > 0) ? (pagNum - 1) : (m_actPage - 1));
					ite->annotation().setAction(QString("%1 %2").arg(xco).arg(yco));
					ite->annotation().setActionType(2);
				}
			}
			else
			{
				POPPLER_CONST GooString *ndst = gto->getNamedDest();
				if (ndst)
				{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
					std::unique_ptr<LinkDest> dstn = pdfDoc->findDest(ndst);
#else
					LinkDest *dstn = pdfDoc->findDest(ndst);
#endif
					if (dstn)
					{
						if (dstn->getKind() == destXYZ)
						{
							if (dstn->isPageRef())
							{
								Ref dstr = dstn->getPageRef();
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 76, 0)
								pagNum = pdfDoc->findPage(dstr);
#else
								pagNum = pdfDoc->findPage(dstr.num, dstr.gen);
#endif
							}
							else
								pagNum = dstn->getPageNum();
							xco = dstn->getLeft();
							yco = dstn->getTop();
							ite->annotation().setZiel((pagNum > 0) ? (pagNum - 1) : (m_actPage - 1));
							ite->annotation().setAction(QString("%1 %2").arg(xco).arg(yco));
							ite->annotation().setActionType(2);
						}
					}
				}
			}
		}
		else if (Lact->getKind() == actionGoToR)
		{
			int pagNum = 0;
			int xco = 0;
			int yco = 0;
			LinkGoToR *gto = (LinkGoToR*)Lact;
			QString fileName = UnicodeParsedString(gto->getFileName());
			POPPLER_CONST LinkDest *dst = gto->getDest();
			if (dst)
			{
				if (dst->getKind() == destXYZ)
				{
					pagNum = dst->getPageNum();
					xco = dst->getLeft();
					yco = dst->getTop();
					ite->annotation().setZiel((pagNum > 0) ? (pagNum - 1) : (m_actPage - 1));
					ite->annotation().setExtern(fileName);
					ite->annotation().setAction(QString("%1 %2").arg(xco).arg(yco));
					ite->annotation().setActionType(9);
				}
			}
			else
			{
				POPPLER_CONST GooString *ndst = gto->getNamedDest();
				if (ndst)
				{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
					std::unique_ptr<LinkDest> dstn = pdfDoc->findDest(ndst);
#else
					LinkDest *dstn = pdfDoc->findDest(ndst);
#endif
					if (dstn)
					{
						if (dstn->getKind() == destXYZ)
						{
							pagNum = dstn->getPageNum();
							xco = dstn->getLeft();
							yco = dstn->getTop();
							ite->annotation().setZiel((pagNum > 0) ? (pagNum - 1) : (m_actPage - 1));
							ite->annotation().setExtern(fileName);
							ite->annotation().setAction(QString("%1 %2").arg(xco).arg(yco));
							ite->annotation().setActionType(9);
						}
					}
				}
			}
		}
		else if (Lact->getKind() == actionUnknown)
		{
			LinkUnknown *uno = (LinkUnknown*)Lact;
			QString actString = UnicodeParsedString(uno->getAction());
			if (actString == "ResetForm")
			{
				ite->annotation().setActionType(4);
			}
			else
			{
				LinkAction* scact = SC_getAction(ano);
				if (scact)
				{
					if (actString == "ImportData")
					{
						LinkImportData *impo = (LinkImportData*)scact;
						if (impo->isOk())
						{
							ite->annotation().setActionType(5);
							ite->annotation().setAction(UnicodeParsedString(impo->getFileName()));
						}
					}
					else if (actString == "SubmitForm")
					{
						LinkSubmitForm *impo = (LinkSubmitForm*)scact;
						if (impo->isOk())
						{
							ite->annotation().setActionType(3);
							ite->annotation().setAction(UnicodeParsedString(impo->getFileName()));
							int fl = impo->getFlags();
							if (fl == 0)
								ite->annotation().setHTML(0);
							else if (fl == 4)
								ite->annotation().setHTML(1);
							else if (fl == 64)
								ite->annotation().setHTML(2);
							else if (fl == 512)
								ite->annotation().setHTML(3);
						}
					}
				}
			}
		}
		else if (Lact->getKind() == actionNamed)
		{
			LinkNamed *uno = (LinkNamed*)Lact;
			ite->annotation().setActionType(10);
			ite->annotation().setAction(UnicodeParsedString(uno->getName()));
		}
		else
			qDebug() << "Found unsupported Action of type" << Lact->getKind();
	}
	auto Aact = SC_getAdditionalAction("D", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setD_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
	Aact = SC_getAdditionalAction("E", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setE_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
	Aact = SC_getAdditionalAction("X", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setX_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
	Aact = SC_getAdditionalAction("Fo", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setFo_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
	Aact = SC_getAdditionalAction("Bl", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setBl_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
	Aact = SC_getAdditionalAction("C", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setC_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
	Aact = SC_getAdditionalAction("F", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setF_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
				ite->annotation().setFormat(5);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
	Aact = SC_getAdditionalAction("K", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setK_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
				ite->annotation().setFormat(5);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
	Aact = SC_getAdditionalAction("V", ano);
	if (Aact)
	{
		if (Aact->getKind() == actionJavaScript)
		{
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
			LinkJavaScript *jsa = (LinkJavaScript*) Aact.get();
#else
			LinkJavaScript *jsa = (LinkJavaScript*) Aact;
#endif
			if (jsa->isOk())
			{
				ite->annotation().setV_act(UnicodeParsedString(jsa->getScript()));
				ite->annotation().setAAact(true);
			}
		}
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 86, 0)
		Aact.reset();
#else
		Aact = nullptr;
#endif
	}
}

void SlaOutputDev::startDoc(PDFDoc *doc, XRef *xrefA, Catalog *catA)
{
	xref = xrefA;
	catalog = catA;
	pdfDoc = doc;
	updateGUICounter = 0;
#if POPPLER_ENCODED_VERSION >= POPPLER_VERSION_ENCODE(0, 84, 0)
	m_fontEngine = new SplashFontEngine(true, false, false, true);
#else
	m_fontEngine = new SplashFontEngine(globalParams->getEnableFreeType(), false, false, true);
#endif
}

void SlaOutputDev::startPage(int pageNum, GfxState *, XRef *)
{
	m_formWidgets = pdfDoc->getPage(pageNum)->getFormWidgets();
	m_radioMap.clear();
	m_radioButtons.clear();
	m_actPage = pageNum;
	m_groupStack.clear();
	pushGroup();
	m_currentClipPath = QPainterPath();
	m_clipPaths.clear();
}

void SlaOutputDev::endPage()
{
	if (!m_radioMap.isEmpty())
	{
		for (auto it = m_radioMap.begin(); it != m_radioMap.end(); ++it)
		{
			tmpSel->clear();
			QList<int> refList = it.value();
			for (int a = 0; a < refList.count(); a++)
			{
				if (m_radioButtons.contains(refList[a]))
				{
					tmpSel->addItem(m_radioButtons[refList[a]], true);
					m_Elements->removeAll(m_radioButtons[refList[a]]);
				}
			}
			if (!tmpSel->isEmpty())
			{
				PageItem *ite = m_doc->groupObjectsSelection(tmpSel);
				ite->setItemName(it.key());
				m_Elements->append(ite);
				if (m_groupStack.count() != 0)
					m_groupStack.top().Items.append(ite);
			}
		}
	}
	m_radioMap.clear();
	m_radioButtons.clear();
//	qDebug() << "ending page";
}

void SlaOutputDev::saveState(GfxState *state)
{
	m_clipPaths.push(m_currentClipPath);
	pushGroup();
}

void SlaOutputDev::restoreState(GfxState *state)
{
	if (m_groupStack.count() != 0)
	{
		groupEntry gElements = m_groupStack.pop();
		if (gElements.Items.count() > 0)
		{
			if ((gElements.Items.count() > 1) && (checkClip()))
			{
				tmpSel->clear();
				for (int dre = 0; dre < gElements.Items.count(); ++dre)
				{
					tmpSel->addItem(gElements.Items.at(dre), true);
					m_Elements->removeAll(gElements.Items.at(dre));
				}
				PageItem *ite = m_doc->groupObjectsSelection(tmpSel);
				if (ite)
				{
					QPainterPath clippath = m_currentClipPath;
					clippath.translate(m_doc->currentPage()->xOffset(), m_doc->currentPage()->yOffset());
					clippath.translate(-ite->xPos(), -ite->yPos());
					ite->PoLine.fromQPainterPath(clippath, true);
					ite->ClipEdited = true;
					ite->FrameType = 3;
					ite->setTextFlowMode(PageItem::TextFlowDisabled);
					// Comment out temporarily, there are some bad interactions between adjustItemSize() and
					// resizeGroupToContents() since fixing resizing of multiple selections
					//m_doc->adjustItemSize(ite, true);
					m_doc->resizeGroupToContents(ite);
					ite->OldB2 = ite->width();
					ite->OldH2 = ite->height();
					m_Elements->append(ite);
					if (m_groupStack.count() != 0)
					{
						applyMask(ite);
						m_groupStack.top().Items.append(ite);
					}
				}
				else
				{
					if (m_groupStack.count() != 0)
					{
						for (int dre = 0; dre < gElements.Items.count(); ++dre)
						{
							PageItem *ite = gElements.Items.at(dre);
							applyMask(ite);
							m_groupStack.top().Items.append(ite);
						}
					}
				}
				tmpSel->clear();
			}
			else
			{
				if (m_groupStack.count() != 0)
				{
					for (int dre = 0; dre < gElements.Items.count(); ++dre)
					{
						PageItem *ite = gElements.Items.at(dre);
						applyMask(ite);
						m_groupStack.top().Items.append(ite);
					}
				}
			}
		}
	}
	if (m_clipPaths.count() != 0)
		m_currentClipPath = m_clipPaths.pop();
}

void SlaOutputDev::beginTransparencyGroup(GfxState *state, POPPLER_CONST_070 double *bbox, GfxColorSpace * /*blendingColorSpace*/, GBool isolated, GBool knockout, GBool forSoftMask)
{
// 	qDebug() << "SlaOutputDev::beginTransparencyGroup isolated:" << isolated << "knockout:" << knockout << "forSoftMask:" << forSoftMask;
	pushGroup("", forSoftMask);
	m_groupStack.top().isolated = isolated;
}

void SlaOutputDev::paintTransparencyGroup(GfxState *state, POPPLER_CONST_070 double *bbox)
{
// 	qDebug() << "SlaOutputDev::paintTransparencyGroup";
	if (m_groupStack.count() != 0)
	{
		if ((m_groupStack.top().Items.count() != 0) && (!m_groupStack.top().forSoftMask))
		{
			PageItem *ite = m_groupStack.top().Items.last();
			ite->setFillTransparency(1.0 - state->getFillOpacity());
			ite->setFillBlendmode(getBlendMode(state));
		}
	}
}

void SlaOutputDev::endTransparencyGroup(GfxState *state)
{
// 	qDebug() << "SlaOutputDev::endTransparencyGroup";
	if (m_groupStack.count() <= 0)
		return;

	tmpSel->clear();

	groupEntry gElements = m_groupStack.pop();
	if (gElements.Items.count() <= 0)
		return;

	if (gElements.forSoftMask)
	{
		for (int dre = 0; dre < gElements.Items.count(); ++dre)
		{
			tmpSel->addItem(gElements.Items.at(dre), true);
			m_Elements->removeAll(gElements.Items.at(dre));
		}
		PageItem *ite = m_doc->groupObjectsSelection(tmpSel);
		ite->setFillTransparency(1.0 - state->getFillOpacity());
		ite->setFillBlendmode(getBlendMode(state));
		ScPattern pat = ScPattern();
		pat.setDoc(m_doc);
		m_doc->DoDrawing = true;
		pat.pattern = ite->DrawObj_toImage(qMin(qMax(ite->width(), ite->height()), 500.0));
		pat.xoffset = 0;
		pat.yoffset = 0;
		m_doc->DoDrawing = false;
		pat.width = ite->width();
		pat.height = ite->height();
		m_currentMaskPosition = QPointF(ite->xPos(), ite->yPos());
		ite->gXpos = 0;
		ite->gYpos = 0;
		ite->setXYPos(ite->gXpos, ite->gYpos, true);
		pat.items.append(ite);
		m_doc->Items->removeAll(ite);
		QString id = QString("Pattern_from_PDF_%1S").arg(m_doc->docPatterns.count() + 1);
		m_doc->addPattern(id, pat);
		m_currentMask = id;
		tmpSel->clear();
		return;
	}
	PageItem *ite;
	for (int dre = 0; dre < gElements.Items.count(); ++dre)
	{
		tmpSel->addItem(gElements.Items.at(dre), true);
		m_Elements->removeAll(gElements.Items.at(dre));
	}
	if ((gElements.Items.count() != 1) || (gElements.isolated))
		ite = m_doc->groupObjectsSelection(tmpSel);
	else
		ite = gElements.Items.first();
	if (ite->isGroup())
	{
		ite->ClipEdited = true;
		ite->FrameType = 3;
		if (checkClip())
		{
			QPainterPath clippath = m_currentClipPath;
			clippath.translate(m_doc->currentPage()->xOffset(), m_doc->currentPage()->yOffset());
			clippath.translate(-ite->xPos(), -ite->yPos());
			ite->PoLine.fromQPainterPath(clippath, true);
			ite->ClipEdited = true;
			ite->FrameType = 3;
			ite->setTextFlowMode(PageItem::TextFlowDisabled);
			// Comment out temporarily, there are some bad interactions between adjustItemSize() and
			// resizeGroupToContents() since fixing resizing of multiple selections
			//m_doc->adjustItemSize(ite, true);
			m_doc->resizeGroupToContents(ite);
			ite->OldB2 = ite->width();
			ite->OldH2 = ite->height();
		}
	}
	ite->setFillTransparency(1.0 - state->getFillOpacity());
	ite->setFillBlendmode(getBlendMode(state));
	m_Elements->append(ite);
	if (m_groupStack.count() != 0)
	{
		applyMask(ite);
		m_groupStack.top().Items.append(ite);
	}

	tmpSel->clear();
}

void SlaOutputDev::setSoftMask(GfxState * /*state*/, POPPLER_CONST_070 double * bbox, GBool alpha, Function *transferFunc, GfxColor * /*backdropColor*/)
{
	if (m_groupStack.count() <= 0)
		return;

	double lum = 0;
	double lum2 = 0;
	if (transferFunc)
		transferFunc->transform(&lum, &lum2);
	else
		lum2 = lum;
	if (lum == lum2)
		m_groupStack.top().inverted = false;
	else
		m_groupStack.top().inverted = true;
	m_groupStack.top().maskName = m_currentMask;
	// Remember the mask's position as it might not align with the image to which the mask is later assigned.
	m_groupStack.top().maskPos = m_currentMaskPosition;
	m_groupStack.top().alpha = alpha;
	if (m_groupStack.top().Items.count() != 0)
		applyMask(m_groupStack.top().Items.last());
}

void SlaOutputDev::clearSoftMask(GfxState * /*state*/)
{
	if (m_groupStack.count() != 0)
		m_groupStack.top().maskName = "";
}

void SlaOutputDev::updateFillColor(GfxState *state)
{
	CurrFillShade = 100;
	CurrColorFill = getColor(state->getFillColorSpace(), state->getFillColor(), &CurrFillShade);
}

void SlaOutputDev::updateStrokeColor(GfxState *state)
{
	CurrStrokeShade = 100;
	CurrColorStroke = getColor(state->getStrokeColorSpace(), state->getStrokeColor(), &CurrStrokeShade);
}

void SlaOutputDev::clip(GfxState *state)
{
//	qDebug() << "Clip";
	adjustClip(state, Qt::WindingFill);
}

void SlaOutputDev::eoClip(GfxState *state)
{
//	qDebug() << "EoClip";
	adjustClip(state, Qt::OddEvenFill);
}

void SlaOutputDev::adjustClip(GfxState *state, Qt::FillRule fillRule)
{
	const double *ctm = state->getCTM();
	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	QString output = convertPath(state->getPath());
	if (output.isEmpty())
		return;
	FPointArray out;
	out.parseSVG(output);
	out.svgClosePath();
	out.map(m_ctm);
	if (checkClip())
	{
		// "clip" (WindingFill) and "eoClip" (OddEvenFill) only the determine
		// the fill rule of the new clipping path. The new clip should be the
		// intersection of the old and new area. QPainterPath determines on
		// its own which fill rule to use for the result. We should not loose
		// this information.
		QPainterPath pathN = out.toQPainterPath(false);
		pathN.setFillRule(fillRule);
		m_currentClipPath = intersection(pathN, m_currentClipPath);
	}
	else
		m_currentClipPath = out.toQPainterPath(false);
}

void SlaOutputDev::stroke(GfxState *state)
{
//	qDebug() << "Stroke";
	const double *ctm;
	ctm = state->getCTM();
	double xCoor = m_doc->currentPage()->xOffset();
	double yCoor = m_doc->currentPage()->yOffset();
	QString output = convertPath(state->getPath());
	getPenState(state);
	if ((m_Elements->count() != 0) && (output == Coords))			// Path is the same as in last fill
	{
		PageItem* ite = m_Elements->last();
		ite->setLineColor(CurrColorStroke);
		ite->setLineShade(CurrStrokeShade);
		ite->setLineEnd(PLineEnd);
		ite->setLineJoin(PLineJoin);
		ite->setLineWidth(state->getTransformedLineWidth());
		ite->setDashes(DashValues);
		ite->setDashOffset(DashOffset);
		ite->setLineTransparency(1.0 - state->getStrokeOpacity());
	}
	else
	{
		FPointArray out;
		out.parseSVG(output);
		m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
		out.map(m_ctm);
		FPoint wh = out.widthHeight();
		if ((out.size() > 3) && ((wh.x() != 0.0) || (wh.y() != 0.0)))
		{
			CurrColorStroke = getColor(state->getStrokeColorSpace(), state->getStrokeColor(), &CurrStrokeShade);
			int z;
			if (pathIsClosed)
				z = m_doc->itemAdd(PageItem::Polygon, PageItem::Unspecified, xCoor, yCoor, 10, 10, state->getTransformedLineWidth(), CommonStrings::None, CurrColorStroke);
			else
				z = m_doc->itemAdd(PageItem::PolyLine, PageItem::Unspecified, xCoor, yCoor, 10, 10, state->getTransformedLineWidth(), CommonStrings::None, CurrColorStroke);
			PageItem* ite = m_doc->Items->at(z);
			ite->PoLine = out.copy();
			ite->ClipEdited = true;
			ite->FrameType = 3;
			ite->setWidthHeight(wh.x(), wh.y());
			m_doc->adjustItemSize(ite);
			if (m_Elements->count() != 0)
			{
				PageItem* lItem = m_Elements->last();
				if ((lItem->lineColor() == CommonStrings::None) && (lItem->PoLine == ite->PoLine))
				{
					lItem->setLineColor(CurrColorStroke);
					lItem->setLineWidth(state->getTransformedLineWidth());
					lItem->setLineShade(CurrStrokeShade);
					lItem->setLineTransparency(1.0 - state->getStrokeOpacity());
					lItem->setLineBlendmode(getBlendMode(state));
					lItem->setLineEnd(PLineEnd);
					lItem->setLineJoin(PLineJoin);
					lItem->setDashes(DashValues);
					lItem->setDashOffset(DashOffset);
					lItem->setTextFlowMode(PageItem::TextFlowDisabled);
					m_doc->Items->removeAll(ite);
				}
				else
				{
					ite->setLineShade(CurrStrokeShade);
					ite->setLineTransparency(1.0 - state->getStrokeOpacity());
					ite->setLineBlendmode(getBlendMode(state));
					ite->setLineEnd(PLineEnd);
					ite->setLineJoin(PLineJoin);
					ite->setDashes(DashValues);
					ite->setDashOffset(DashOffset);
					ite->setTextFlowMode(PageItem::TextFlowDisabled);
					m_Elements->append(ite);
					if (m_groupStack.count() != 0)
						m_groupStack.top().Items.append(ite);
				}
			}
			else
			{
				ite->setLineShade(CurrStrokeShade);
				ite->setLineTransparency(1.0 - state->getStrokeOpacity());
				ite->setLineBlendmode(getBlendMode(state));
				ite->setLineEnd(PLineEnd);
				ite->setLineJoin(PLineJoin);
				ite->setDashes(DashValues);
				ite->setDashOffset(DashOffset);
				ite->setTextFlowMode(PageItem::TextFlowDisabled);
				m_Elements->append(ite);
				if (m_groupStack.count() != 0)
					m_groupStack.top().Items.append(ite);
			}
		}
	}
}

void SlaOutputDev::fill(GfxState *state)
{
//	qDebug() << "Fill";
	createFillItem(state, Qt::WindingFill);
}

void SlaOutputDev::eoFill(GfxState *state)
{
//	qDebug() << "EoFill";
	createFillItem(state, Qt::OddEvenFill);
}

void SlaOutputDev::createFillItem(GfxState *state, Qt::FillRule fillRule)
{
	const double *ctm;
	ctm = state->getCTM();
	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	double xCoor = m_doc->currentPage()->xOffset();
	double yCoor = m_doc->currentPage()->yOffset();
	FPointArray out;
	QString output = convertPath(state->getPath());
	out.parseSVG(output);
	out.map(m_ctm);

	// Clip the new path first and only add it if it is not empty.
	QPainterPath path = out.toQPainterPath(false);
	path.setFillRule(fillRule);
	QPainterPath clippedPath = intersection(m_currentClipPath, path);

	// Undo the rotation of the clipping path as it is rotated together with the item.
	double angle = m_ctm.map(QLineF(0, 0, 1, 0)).angle();
	QTransform mm;
	mm.rotate(angle);
	clippedPath = mm.map(clippedPath);

	Coords = output;
	QRectF bbox = clippedPath.boundingRect();
	if (!clippedPath.isEmpty() && !bbox.isNull())
	{
		CurrColorFill = getColor(state->getFillColorSpace(), state->getFillColor(), &CurrFillShade);
		int z;
		if (pathIsClosed)
			z = m_doc->itemAdd(PageItem::Polygon, PageItem::Unspecified, xCoor, yCoor, 10, 10, 0, CurrColorFill, CommonStrings::None);
		else
			z = m_doc->itemAdd(PageItem::PolyLine, PageItem::Unspecified, xCoor, yCoor, 10, 10, 0, CurrColorFill, CommonStrings::None);
		PageItem* ite = m_doc->Items->at(z);
		ite->PoLine.fromQPainterPath(clippedPath, true);
		ite->ClipEdited = true;
		ite->FrameType = 3;
		ite->setFillShade(CurrFillShade);
		ite->setLineShade(100);
		ite->setRotation(-angle);
		// Only the new path has to be interpreted according to fillRule. QPainterPath
		// could decide to create a final path according to the other rule. Thus
		// we have to set this from the final path.
		ite->setFillEvenOdd(clippedPath.fillRule() == Qt::OddEvenFill);
		ite->setFillTransparency(1.0 - state->getFillOpacity());
		ite->setFillBlendmode(getBlendMode(state));
		ite->setLineEnd(PLineEnd);
		ite->setLineJoin(PLineJoin);
		ite->setWidthHeight(bbox.width(),bbox.height());
		ite->setTextFlowMode(PageItem::TextFlowDisabled);
		m_doc->adjustItemSize(ite);
		m_Elements->append(ite);
		if (m_groupStack.count() != 0)
		{
			m_groupStack.top().Items.append(ite);
			applyMask(ite);
		}
	}
}

GBool SlaOutputDev::axialShadedFill(GfxState *state, GfxAxialShading *shading, double tMin, double tMax)
{
//	qDebug() << "SlaOutputDev::axialShadedFill";
	double GrStartX;
	double GrStartY;
	double GrEndX;
	double GrEndY;
	int shade = 100;
	POPPLER_CONST_070 Function *func = shading->getFunc(0);
	VGradient FillGradient = VGradient(VGradient::linear);
	FillGradient.clearStops();
	GfxColorSpace *color_space = shading->getColorSpace();
	if (func->getType() == 3)
	{
		StitchingFunction *stitchingFunc = (StitchingFunction*)func;
		const double *bounds = stitchingFunc->getBounds();
		int num_funcs = stitchingFunc->getNumFuncs();
		double domain_min = stitchingFunc->getDomainMin(0);
		double domain_max = stitchingFunc->getDomainMax(0);
		if (fabs(domain_max - domain_min) < 1e-6)
		{
			domain_min = 0.0;
			domain_max = 1.0;
		}
		// Add stops from all the stitched functions
		for (int i = 0 ; i <= num_funcs ; i++)
		{
			GfxColor temp;
			shading->getColor(bounds[i], &temp);
			QString stopColor = getColor(color_space, &temp, &shade);
			double stopPoint = (bounds[i] - domain_min) / (domain_max - domain_min);
			FillGradient.addStop( ScColorEngine::getShadeColor(m_doc->PageColors[stopColor], m_doc, shade), stopPoint, 0.5, 1.0, stopColor, shade );
		}
	}
	else if ((func->getType() == 2) || (func->getType() == 0))
	{
		GfxColor stop1;
		shading->getColor(0.0, &stop1);
		QString stopColor1 = getColor(color_space, &stop1, &shade);
		FillGradient.addStop( ScColorEngine::getShadeColor(m_doc->PageColors[stopColor1], m_doc, shade), 0.0, 0.5, 1.0, stopColor1, shade );
		GfxColor stop2;
		shading->getColor(1.0, &stop2);
		QString stopColor2 = getColor(color_space, &stop2, &shade);
		FillGradient.addStop( ScColorEngine::getShadeColor(m_doc->PageColors[stopColor2], m_doc, shade), 1.0, 0.5, 1.0, stopColor2, shade );
	}
	shading->getCoords(&GrStartX, &GrStartY, &GrEndX, &GrEndY);
	double xmin, ymin, xmax, ymax;
	// get the clip region bbox
	state->getClipBBox(&xmin, &ymin, &xmax, &ymax);
	QRectF crect = QRectF(QPointF(xmin, ymin), QPointF(xmax, ymax));
	crect = crect.normalized();
	QPainterPath out;
	out.addRect(crect);
	if (checkClip())
	{
		// Apply the clip path early to adjust the gradient vector to the
		// smaller boundign box.
		out = intersection(m_currentClipPath, out);
		crect = out.boundingRect();
	}
	const double *ctm = state->getCTM();
	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	FPointArray gr;
	gr.addPoint(GrStartX, GrStartY);
	gr.addPoint(GrEndX, GrEndY);
	gr.map(m_ctm);
	gr.translate(-crect.x(), -crect.y());

	// Undo the rotation and translation of the gradient vector.
	double angle = m_ctm.map(QLineF(0, 0, 1, 0)).angle();
	QTransform mm;
	mm.rotate(angle);
	out.translate(-crect.x(), -crect.y());
	out = mm.map(out);
	QRectF bb = out.boundingRect();
	gr.map(mm);
	gr.translate(-bb.left(), -bb.top());
	GrStartX = gr.point(0).x();
	GrStartY = gr.point(0).y();
	GrEndX = gr.point(1).x();
	GrEndY = gr.point(1).y();

	double xCoor = m_doc->currentPage()->xOffset();
	double yCoor = m_doc->currentPage()->yOffset();
	QString output = QString("M %1 %2").arg(0.0).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(0.0);
	output += QString("Z");
	pathIsClosed = true;
	Coords = output;
	int z = m_doc->itemAdd(PageItem::Polygon, PageItem::Rectangle, xCoor + crect.x(), yCoor + crect.y(), bb.width(), bb.height(), 0, CurrColorFill, CommonStrings::None);
	PageItem* ite = m_doc->Items->at(z);
	if (checkClip())
	{
		ite->PoLine.fromQPainterPath(out, true);
		ite->setFillEvenOdd(out.fillRule() == Qt::OddEvenFill);
	}
	ite->setRotation(-angle);
	ite->ClipEdited = true;
	ite->FrameType = 3;
	ite->setFillShade(CurrFillShade);
	ite->setLineShade(100);
	ite->setFillTransparency(1.0 - state->getFillOpacity());
	ite->setFillBlendmode(getBlendMode(state));
	ite->setLineEnd(PLineEnd);
	ite->setLineJoin(PLineJoin);
	ite->setTextFlowMode(PageItem::TextFlowDisabled);
	ite->GrType = 6;
	if (!shading->getExtend0() || !shading->getExtend1())
	{
		FillGradient.setRepeatMethod(VGradient::none);
		ite->setGradientExtend(VGradient::none);
	}
	else
	{
		FillGradient.setRepeatMethod(VGradient::pad);
		ite->setGradientExtend(VGradient::pad);
	}
	ite->fill_gradient = FillGradient;
	ite->setGradientVector(GrStartX, GrStartY, GrEndX, GrEndY, 0, 0, 1, 0);
	m_doc->adjustItemSize(ite);
	m_Elements->append(ite);
	if (m_groupStack.count() != 0)
	{
		m_groupStack.top().Items.append(ite);
		applyMask(ite);
	}
	return gTrue;
}

GBool SlaOutputDev::radialShadedFill(GfxState *state, GfxRadialShading *shading, double sMin, double sMax)
{
//	qDebug() << "SlaOutputDev::radialShadedFill";
	double GrStartX;
	double GrStartY;
	double GrEndX;
	double GrEndY;
	int shade = 100;
	POPPLER_CONST_070 Function *func = shading->getFunc(0);
	VGradient FillGradient = VGradient(VGradient::linear);
	FillGradient.clearStops();
	GfxColorSpace *color_space = shading->getColorSpace();
	if (func->getType() == 3)
	{
		StitchingFunction *stitchingFunc = (StitchingFunction*)func;
		const double *bounds = stitchingFunc->getBounds();
		int num_funcs = stitchingFunc->getNumFuncs();
		double domain_min = stitchingFunc->getDomainMin(0);
		double domain_max = stitchingFunc->getDomainMax(0);
		if (fabs(domain_max - domain_min) < 1e-6)
		{
			domain_min = 0.0;
			domain_max = 1.0;
		}
		// Add stops from all the stitched functions
		for (int i = 0 ; i <= num_funcs ; i++)
		{
			GfxColor temp;
			shading->getColor(bounds[i], &temp);
			QString stopColor = getColor(color_space, &temp, &shade);
			double stopPoint = (bounds[i] - domain_min) / (domain_max - domain_min);
			FillGradient.addStop( ScColorEngine::getShadeColor(m_doc->PageColors[stopColor], m_doc, shade), stopPoint, 0.5, 1.0, stopColor, shade );
		}
	}
	else if ((func->getType() == 2) || (func->getType() == 0))
	{
		GfxColor stop1;
		shading->getColor(0.0, &stop1);
		QString stopColor1 = getColor(color_space, &stop1, &shade);
		FillGradient.addStop( ScColorEngine::getShadeColor(m_doc->PageColors[stopColor1], m_doc, shade), 0.0, 0.5, 1.0, stopColor1, shade );
		GfxColor stop2;
		shading->getColor(1.0, &stop2);
		QString stopColor2 = getColor(color_space, &stop2, &shade);
		FillGradient.addStop( ScColorEngine::getShadeColor(m_doc->PageColors[stopColor2], m_doc, shade), 1.0, 0.5, 1.0, stopColor2, shade );
	}
	double r0, x1, y1, r1;
	shading->getCoords(&GrStartX, &GrStartY, &r0, &x1, &y1, &r1);
	double xmin, ymin, xmax, ymax;
	// get the clip region bbox
	state->getClipBBox(&xmin, &ymin, &xmax, &ymax);
	QRectF crect = QRectF(QPointF(xmin, ymin), QPointF(xmax, ymax));
	crect = crect.normalized();
	double GrFocalX = x1;
	double GrFocalY = y1;
	GrEndX = GrFocalX + r1;
	GrEndY = GrFocalY;
	const double *ctm = state->getCTM();
	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	FPointArray gr;
	gr.addPoint(GrStartX, GrStartY);
	gr.addPoint(GrEndX, GrEndY);
	gr.addPoint(GrFocalX, GrFocalY);
	gr.map(m_ctm);
	GrStartX = gr.point(0).x() - crect.x();
	GrStartY = gr.point(0).y() - crect.y();
	GrEndX = gr.point(1).x() - crect.x();
	GrEndY = gr.point(1).y() - crect.y();
	GrFocalX = gr.point(2).x() - crect.x();
	GrFocalY = gr.point(2).y() - crect.y();
	double xCoor = m_doc->currentPage()->xOffset();
	double yCoor = m_doc->currentPage()->yOffset();
	QString output = QString("M %1 %2").arg(0.0).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(0.0);
	output += QString("Z");
	pathIsClosed = true;
	Coords = output;
	int z = m_doc->itemAdd(PageItem::Polygon, PageItem::Rectangle, xCoor + crect.x(), yCoor + crect.y(), crect.width(), crect.height(), 0, CurrColorFill, CommonStrings::None);
	PageItem* ite = m_doc->Items->at(z);
	if (checkClip())
	{
		QPainterPath out = m_currentClipPath;
		out.translate(m_doc->currentPage()->xOffset(), m_doc->currentPage()->yOffset());
		out.translate(-ite->xPos(), -ite->yPos());
		ite->PoLine.fromQPainterPath(out, true);
		ite->setFillEvenOdd(out.fillRule() == Qt::OddEvenFill);
	}
	ite->ClipEdited = true;
	ite->FrameType = 3;
	ite->setFillShade(CurrFillShade);
	ite->setLineShade(100);
	ite->setFillTransparency(1.0 - state->getFillOpacity());
	ite->setFillBlendmode(getBlendMode(state));
	ite->setLineEnd(PLineEnd);
	ite->setLineJoin(PLineJoin);
	ite->setTextFlowMode(PageItem::TextFlowDisabled);
	ite->GrType = 7;
	if (!shading->getExtend0() || !shading->getExtend1())
	{
		FillGradient.setRepeatMethod(VGradient::none);
		ite->setGradientExtend(VGradient::none);
	}
	else
	{
		FillGradient.setRepeatMethod(VGradient::pad);
		ite->setGradientExtend(VGradient::pad);
	}
	ite->fill_gradient = FillGradient;
	ite->setGradientVector(GrStartX, GrStartY, GrEndX, GrEndY, GrFocalX, GrFocalY, 1, 0);
	m_doc->adjustItemSize(ite);
	m_Elements->append(ite);
	if (m_groupStack.count() != 0)
	{
		m_groupStack.top().Items.append(ite);
		applyMask(ite);
	}
	return gTrue;
}

GBool SlaOutputDev::gouraudTriangleShadedFill(GfxState *state, GfxGouraudTriangleShading *shading)
{
//	qDebug() << "SlaOutputDev::gouraudTriangleShadedFill";
	double xCoor = m_doc->currentPage()->xOffset();
	double yCoor = m_doc->currentPage()->yOffset();
	double xmin, ymin, xmax, ymax;
	// get the clip region bbox
	state->getClipBBox(&xmin, &ymin, &xmax, &ymax);
	QRectF crect = QRectF(QPointF(xmin, ymin), QPointF(xmax, ymax));
	crect = crect.normalized();
	QString output = QString("M %1 %2").arg(0.0).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(0.0);
	output += QString("Z");
	pathIsClosed = true;
	Coords = output;
	const double *ctm = state->getCTM();
	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	int z = m_doc->itemAdd(PageItem::Polygon, PageItem::Rectangle, xCoor + crect.x(), yCoor + crect.y(), crect.width(), crect.height(), 0, CurrColorFill, CommonStrings::None);
	PageItem* ite = m_doc->Items->at(z);
	ite->ClipEdited = true;
	ite->FrameType = 3;
	ite->setFillShade(CurrFillShade);
	ite->setLineShade(100);
	ite->setFillEvenOdd(false);
	ite->setFillTransparency(1.0 - state->getFillOpacity());
	ite->setFillBlendmode(getBlendMode(state));
	ite->setLineEnd(PLineEnd);
	ite->setLineJoin(PLineJoin);
	ite->setTextFlowMode(PageItem::TextFlowDisabled);
	m_doc->adjustItemSize(ite);
	m_Elements->append(ite);
	if (m_groupStack.count() != 0)
	{
		m_groupStack.top().Items.append(ite);
		applyMask(ite);
	}
	GfxColor color[3];
	double x0, y0, x1, y1, x2, y2;
	for (int i = 0; i < shading->getNTriangles(); i++)
	{
		int shade = 100;
		meshGradientPatch patchM;
		shading->getTriangle(i, &x0, &y0, &color[0],  &x1, &y1, &color[1],  &x2, &y2, &color[2]);
		patchM.BL.resetTo(FPoint(x0, y0));
		patchM.BL.shade = 100;
		patchM.BL.transparency = 1.0;
		patchM.BL.colorName = getColor(shading->getColorSpace(), &color[0], &shade);
		patchM.BL.color = ScColorEngine::getShadeColorProof(m_doc->PageColors[patchM.BL.colorName], m_doc, shade);
		patchM.TL.resetTo(FPoint(x1, y1));
		patchM.TL.shade = 100;
		patchM.TL.transparency = 1.0;
		patchM.TL.colorName = getColor(shading->getColorSpace(), &color[1], &shade);
		patchM.TL.color = ScColorEngine::getShadeColorProof(m_doc->PageColors[patchM.TL.colorName], m_doc, shade);
		patchM.TR.resetTo(FPoint(x2, y2));
		patchM.TR.shade = 100;
		patchM.TR.transparency = 1.0;
		patchM.TR.colorName = getColor(shading->getColorSpace(), &color[2], &shade);
		patchM.TR.color = ScColorEngine::getShadeColorProof(m_doc->PageColors[patchM.TR.colorName], m_doc, shade);
		patchM.BR.resetTo(FPoint(x0, y0));
		patchM.BR.shade = 100;
		patchM.BR.transparency = 1.0;
		patchM.BR.colorName = getColor(shading->getColorSpace(), &color[0], &shade);
		patchM.BR.color = ScColorEngine::getShadeColorProof(m_doc->PageColors[patchM.BR.colorName], m_doc, shade);
		patchM.TL.transform(m_ctm);
		patchM.TL.moveRel(-crect.x(), -crect.y());
		patchM.TR.transform(m_ctm);
		patchM.TR.moveRel(-crect.x(), -crect.y());
		patchM.BR.transform(m_ctm);
		patchM.BR.moveRel(-crect.x(), -crect.y());
		patchM.BL.transform(m_ctm);
		patchM.BL.moveRel(-crect.x(), -crect.y());
		ite->meshGradientPatches.append(patchM);
	}
	ite->GrType = 12;
	return gTrue;
}

GBool SlaOutputDev::patchMeshShadedFill(GfxState *state, GfxPatchMeshShading *shading)
{
//	qDebug() << "SlaOutputDev::patchMeshShadedFill";
	double xCoor = m_doc->currentPage()->xOffset();
	double yCoor = m_doc->currentPage()->yOffset();
	double xmin, ymin, xmax, ymax;
	// get the clip region bbox
	state->getClipBBox(&xmin, &ymin, &xmax, &ymax);
	QRectF crect = QRectF(QPointF(xmin, ymin), QPointF(xmax, ymax));
	crect = crect.normalized();
	QString output = QString("M %1 %2").arg(0.0).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(0.0);
	output += QString("Z");
	pathIsClosed = true;
	Coords = output;
	const double *ctm = state->getCTM();
	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	int z = m_doc->itemAdd(PageItem::Polygon, PageItem::Rectangle, xCoor + crect.x(), yCoor + crect.y(), crect.width(), crect.height(), 0, CurrColorFill, CommonStrings::None);
	PageItem* ite = m_doc->Items->at(z);
	ite->ClipEdited = true;
	ite->FrameType = 3;
	ite->setFillShade(CurrFillShade);
	ite->setLineShade(100);
	ite->setFillEvenOdd(false);
	ite->setFillTransparency(1.0 - state->getFillOpacity());
	ite->setFillBlendmode(getBlendMode(state));
	ite->setLineEnd(PLineEnd);
	ite->setLineJoin(PLineJoin);
	ite->setTextFlowMode(PageItem::TextFlowDisabled);
	m_doc->adjustItemSize(ite);
	m_Elements->append(ite);
	if (m_groupStack.count() != 0)
	{
		m_groupStack.top().Items.append(ite);
		applyMask(ite);
	}
	ite->meshGradientPatches.clear();
	for (int i = 0; i < shading->getNPatches(); i++)
	{
		int shade = 100;
		const GfxPatch *patch = shading->getPatch(i);
		GfxColor color;
		meshGradientPatch patchM;
		int u, v;
		patchM.BL.resetTo(FPoint(patch->x[0][0], patch->y[0][0]));
		patchM.BL.controlTop = FPoint(patch->x[0][1], patch->y[0][1]);
		patchM.BL.controlRight = FPoint(patch->x[1][0], patch->y[1][0]);
		patchM.BL.controlColor = FPoint(patch->x[1][1], patch->y[1][1]);
		u = 0;
		v = 0;
		if (shading->isParameterized())
		{
			shading->getParameterizedColor (patch->color[u][v].c[0], &color);
		}
		else
		{
			for (int k = 0; k < shading->getColorSpace()->getNComps(); k++)
			{
				color.c[k] = GfxColorComp (patch->color[u][v].c[k]);
			}
		}
		patchM.BL.colorName = getColor(shading->getColorSpace(), &color, &shade);
		patchM.BL.shade = 100;
		patchM.BL.transparency = 1.0;
		patchM.BL.color = ScColorEngine::getShadeColorProof(m_doc->PageColors[patchM.BL.colorName], m_doc, shade);

		u = 0;
		v = 1;
		patchM.TL.resetTo(FPoint(patch->x[0][3], patch->y[0][3]));
		patchM.TL.controlRight = FPoint(patch->x[1][3], patch->y[1][3]);
		patchM.TL.controlBottom = FPoint(patch->x[0][2], patch->y[0][2]);
		patchM.TL.controlColor = FPoint(patch->x[1][2], patch->y[1][2]);
		if (shading->isParameterized())
		{
			shading->getParameterizedColor (patch->color[u][v].c[0], &color);
		}
		else
		{
			for (int k = 0; k < shading->getColorSpace()->getNComps(); k++)
			{
				color.c[k] = GfxColorComp (patch->color[u][v].c[k]);
			}
		}
		patchM.TL.colorName = getColor(shading->getColorSpace(), &color, &shade);
		patchM.TL.shade = 100;
		patchM.TL.transparency = 1.0;
		patchM.TL.color = ScColorEngine::getShadeColorProof(m_doc->PageColors[patchM.TL.colorName], m_doc, shade);

		u = 1;
		v = 1;
		patchM.TR.resetTo(FPoint(patch->x[3][3], patch->y[3][3]));
		patchM.TR.controlBottom = FPoint(patch->x[3][2], patch->y[3][2]);
		patchM.TR.controlLeft = FPoint(patch->x[2][3], patch->y[2][3]);
		patchM.TR.controlColor = FPoint(patch->x[2][2], patch->y[2][2]);
		if (shading->isParameterized())
		{
			shading->getParameterizedColor (patch->color[u][v].c[0], &color);
		}
		else
		{
			for (int k = 0; k < shading->getColorSpace()->getNComps(); k++)
			{
				color.c[k] = GfxColorComp (patch->color[u][v].c[k]);
			}
		}
		patchM.TR.colorName = getColor(shading->getColorSpace(), &color, &shade);
		patchM.TR.shade = 100;
		patchM.TR.transparency = 1.0;
		patchM.TR.color = ScColorEngine::getShadeColorProof(m_doc->PageColors[patchM.TR.colorName], m_doc, shade);

		u = 1;
		v = 0;
		patchM.BR.resetTo(FPoint(patch->x[3][0], patch->y[3][0]));
		patchM.BR.controlLeft = FPoint(patch->x[2][0], patch->y[2][0]);
		patchM.BR.controlTop = FPoint(patch->x[3][1], patch->y[3][1]);
		patchM.BR.controlColor = FPoint(patch->x[2][1], patch->y[2][1]);
		if (shading->isParameterized())
		{
			shading->getParameterizedColor (patch->color[u][v].c[0], &color);
		}
		else
		{
			for (int k = 0; k < shading->getColorSpace()->getNComps(); k++)
			{
				color.c[k] = GfxColorComp (patch->color[u][v].c[k]);
			}
		}
		patchM.BR.colorName = getColor(shading->getColorSpace(), &color, &shade);
		patchM.BR.shade = 100;
		patchM.BR.transparency = 1.0;
		patchM.BR.color = ScColorEngine::getShadeColorProof(m_doc->PageColors[patchM.BR.colorName], m_doc, shade);

		patchM.TL.transform(m_ctm);
		patchM.TL.moveRel(-crect.x(), -crect.y());
		patchM.TR.transform(m_ctm);
		patchM.TR.moveRel(-crect.x(), -crect.y());
		patchM.BR.transform(m_ctm);
		patchM.BR.moveRel(-crect.x(), -crect.y());
		patchM.BL.transform(m_ctm);
		patchM.BL.moveRel(-crect.x(), -crect.y());
		ite->meshGradientPatches.append(patchM);
	}
	ite->GrType = 12;
	return gTrue;
}

GBool SlaOutputDev::tilingPatternFill(GfxState *state, Gfx * /*gfx*/, Catalog *cat, Object *str, POPPLER_CONST_070 double *pmat, int paintType, int tilingType, Dict *resDict, POPPLER_CONST_070 double *mat, POPPLER_CONST_070 double *bbox, int x0, int y0, int x1, int y1, double xStep, double yStep)
{
//	qDebug() << "SlaOutputDev::tilingPatternFill";
	PDFRectangle box;
	Gfx *gfx;
	QString id;
	PageItem *ite;
	groupEntry gElements;
	gElements.forSoftMask = gFalse;
	gElements.alpha = gFalse;
	gElements.inverted = false;
	gElements.maskName = "";
	gElements.Items.clear();
	m_groupStack.push(gElements);
	double width, height;
	width = bbox[2] - bbox[0];
	height = bbox[3] - bbox[1];
	if (xStep != width || yStep != height)
		return gFalse;
	box.x1 = bbox[0];
	box.y1 = bbox[1];
	box.x2 = bbox[2];
	box.y2 = bbox[3];

	const double *ctm = state->getCTM();
	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	QTransform mm = QTransform(mat[0], mat[1], mat[2], mat[3], mat[4], mat[5]);
	QTransform mmx = mm * m_ctm;

	gfx = new Gfx(pdfDoc, this, resDict, &box, nullptr);
	inPattern++;
	// Unset the clip path as it is unrelated to the pattern's coordinate space.
	QPainterPath savedClip = m_currentClipPath;
	m_currentClipPath = QPainterPath();
	gfx->display(str);
	m_currentClipPath = savedClip;
	inPattern--;
	gElements = m_groupStack.pop();
	m_doc->m_Selection->clear();
//	double pwidth = 0;
//	double pheight = 0;
	if (gElements.Items.count() > 0)
	{
		for (int dre = 0; dre < gElements.Items.count(); ++dre)
		{
			m_doc->m_Selection->addItem(gElements.Items.at(dre), true);
			m_Elements->removeAll(gElements.Items.at(dre));
		}
		m_doc->itemSelection_FlipV();
		PageItem *ite;
		if (m_doc->m_Selection->count() > 1)
			ite = m_doc->groupObjectsSelection();
		else
			ite = m_doc->m_Selection->itemAt(0);
		ite->setFillTransparency(1.0 - state->getFillOpacity());
		ite->setFillBlendmode(getBlendMode(state));
		m_doc->m_Selection->clear();
		ScPattern pat = ScPattern();
		pat.setDoc(m_doc);
		m_doc->DoDrawing = true;
		pat.pattern = ite->DrawObj_toImage(qMin(qMax(ite->width(), ite->height()), 500.0));
		pat.xoffset = 0;
		pat.yoffset = 0;
		m_doc->DoDrawing = false;
		pat.width = ite->width();
		pat.height = ite->height();
	//	pwidth = ite->width();
	//	pheight = ite->height();
		ite->gXpos = 0;
		ite->gYpos = 0;
		ite->setXYPos(ite->gXpos, ite->gYpos, true);
		pat.items.append(ite);
		m_doc->Items->removeAll(ite);
		id = QString("Pattern_from_PDF_%1").arg(m_doc->docPatterns.count() + 1);
		m_doc->addPattern(id, pat);
	}
	double xCoor = m_doc->currentPage()->xOffset();
	double yCoor = m_doc->currentPage()->yOffset();
	double xmin, ymin, xmax, ymax;
	// get the clip region bbox
	state->getClipBBox(&xmin, &ymin, &xmax, &ymax);
	QRectF crect = QRectF(QPointF(xmin, ymin), QPointF(xmax, ymax));
	crect = crect.normalized();
	QString output = QString("M %1 %2").arg(0.0).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(0.0);
	output += QString("L %1 %2").arg(crect.width()).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(crect.height());
	output += QString("L %1 %2").arg(0.0).arg(0.0);
	output += QString("Z");
	pathIsClosed = true;
	Coords = output;
	int z = m_doc->itemAdd(PageItem::Polygon, PageItem::Rectangle, xCoor + crect.x(), yCoor + crect.y(), crect.width(), crect.height(), 0, CurrColorFill, CommonStrings::None);
	ite = m_doc->Items->at(z);

	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	double angle = m_ctm.map(QLineF(0, 0, 1, 0)).angle();
	ite->setRotation(-angle);
	if (checkClip())
	{
		QPainterPath outline = m_currentClipPath;
		outline.translate(xCoor - ite->xPos(), yCoor - ite->yPos());
		// Undo the rotation of the clipping path as it is rotated together with the item.
		QTransform mm;
		mm.rotate(angle);
		outline = mm.map(outline);
		ite->PoLine.fromQPainterPath(outline, true);
		ite->setFillEvenOdd(outline.fillRule() == Qt::OddEvenFill);
	}
	ite->ClipEdited = true;
	ite->FrameType = 3;
	ite->setFillShade(CurrFillShade);
	ite->setLineShade(100);
	ite->setFillTransparency(1.0 - state->getFillOpacity());
	ite->setFillBlendmode(getBlendMode(state));
	ite->setLineEnd(PLineEnd);
	ite->setLineJoin(PLineJoin);
	ite->setTextFlowMode(PageItem::TextFlowDisabled);
	ite->GrType = 8;
	ite->setPattern(id);
	ite->setPatternTransform(fabs(pmat[0]) * 100, fabs(pmat[3]) * 100, mmx.dx() - ctm[4], mmx.dy() - ctm[5], 0, -1 * pmat[1], pmat[2]);
	m_doc->adjustItemSize(ite);
	m_Elements->append(ite);
	if (m_groupStack.count() != 0)
	{
		m_groupStack.top().Items.append(ite);
		applyMask(ite);
	}
	delete gfx;
	return gTrue;
}

void SlaOutputDev::drawImageMask(GfxState *state, Object *ref, Stream *str, int width, int height, GBool invert, GBool interpolate, GBool inlineImg)
{
//	qDebug() << "Draw Image Mask";
	QImage * image = nullptr;
	int invert_bit;
	int row_stride;
	int x, y, i, bit;
	unsigned char *dest = nullptr;
	unsigned char *buffer;
	Guchar *pix;
	ImageStream * imgStr = new ImageStream(str, width, 1, 1);
	imgStr->reset();
#ifdef WORDS_BIGENDIAN
	image = new QImage(width, height, QImage::Format_Mono);
#else
	image = new QImage(width, height, QImage::Format_MonoLSB);
#endif
	if (image == nullptr || image->isNull())
	{
		delete imgStr;
		delete image;
		return;
	}
	invert_bit = invert ? 1 : 0;
	buffer = image->bits();
	row_stride = image->bytesPerLine();
	for (y = 0; y < height; y++)
	{
		pix = imgStr->getLine();
		dest = buffer + y * row_stride;
		i = 0;
		bit = 0;
		for (x = 0; x < width; x++)
		{
			if (bit == 0)
				dest[i] = 0;
			if (!(pix[x] ^ invert_bit))
			{
#ifdef WORDS_BIGENDIAN
				dest[i] |= (1 << (7 - bit));
#else
				dest[i] |= (1 << bit);
#endif
			}
			bit++;
			if (bit > 7)
			{
				bit = 0;
				i++;
			}
		}
	}
	QColor backColor = ScColorEngine::getShadeColorProof(m_doc->PageColors[CurrColorFill], m_doc, CurrFillShade);
	QImage res = QImage(width, height, QImage::Format_ARGB32);
	res.fill(backColor.rgb());
	unsigned char cc, cm, cy, ck;
	for (int yi = 0; yi < res.height(); ++yi)
	{
		QRgb *t = (QRgb*)(res.scanLine( yi ));
		for (int xi = 0; xi < res.width(); ++xi)
		{
			cc = qRed(*t);
			cm = qGreen(*t);
			cy = qBlue(*t);
			ck = image->pixel(xi, yi);
			if (ck == 0)
				(*t) = qRgba(cc, cm, cy, 0);
			else
				(*t) = qRgba(cc, cm, cy, 255);
			t++;
		}
	}

	createImageFrame(res, state, 3);

	imgStr->close();
	delete imgStr;
	delete image;
}

void SlaOutputDev::drawSoftMaskedImage(GfxState *state, Object *ref, Stream *str, int width, int height, GfxImageColorMap *colorMap, GBool interpolate, Stream *maskStr, int maskWidth, int maskHeight,
				   GfxImageColorMap *maskColorMap, GBool maskInterpolate)
{
//	qDebug() << "SlaOutputDev::drawSoftMaskedImage Masked Image Components" << colorMap->getNumPixelComps();
	ImageStream * imgStr = new ImageStream(str, width, colorMap->getNumPixelComps(), colorMap->getBits());
	imgStr->reset();
	unsigned int *dest = nullptr;
	unsigned char * buffer = new unsigned char[width * height * 4];
	QImage * image = nullptr;
	for (int y = 0; y < height; y++)
	{
		dest = (unsigned int *)(buffer + y * 4 * width);
		Guchar * pix = imgStr->getLine();
		colorMap->getRGBLine(pix, dest, width);
	}
	image = new QImage(buffer, width, height, QImage::Format_RGB32);
	if (image == nullptr || image->isNull())
	{
		delete imgStr;
		delete[] buffer;
		delete image;
		return;
	}
	ImageStream *mskStr = new ImageStream(maskStr, maskWidth, maskColorMap->getNumPixelComps(), maskColorMap->getBits());
	mskStr->reset();
	Guchar *mdest = nullptr;
	unsigned char * mbuffer = new unsigned char[maskWidth * maskHeight];
	memset(mbuffer, 0, maskWidth * maskHeight);
	for (int y = 0; y < maskHeight; y++)
	{
		mdest = (Guchar *)(mbuffer + y * maskWidth);
		Guchar * pix = mskStr->getLine();
		maskColorMap->getGrayLine(pix, mdest, maskWidth);
	}
	if ((maskWidth != width) || (maskHeight != height))
		*image = image->scaled(maskWidth, maskHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	QImage res = image->convertToFormat(QImage::Format_ARGB32);

	int matteRc, matteGc, matteBc;
	POPPLER_CONST_070 GfxColor *matteColor = maskColorMap->getMatteColor();
	if (matteColor != nullptr)
	{
		GfxRGB matteRgb;
		colorMap->getColorSpace()->getRGB(matteColor, &matteRgb);
		matteRc = qRound(colToDbl(matteRgb.r) * 255);
		matteGc = qRound(colToDbl(matteRgb.g) * 255);
		matteBc = qRound(colToDbl(matteRgb.b) * 255);
	}

	unsigned char cr, cg, cb, ca;
	int s = 0;
	for (int yi=0; yi < res.height(); ++yi)
	{
		QRgb *t = (QRgb*)(res.scanLine( yi ));
		for (int xi=0; xi < res.width(); ++xi)
		{
			cr = qRed(*t);
			cg = qGreen(*t);
			cb = qBlue(*t);
			ca = mbuffer[s];
			if (matteColor != nullptr)
			{
				cr = unblendMatte(cr, ca, matteRc);
				cg = unblendMatte(cg, ca, matteGc);
				cb = unblendMatte(cb, ca, matteBc);
			}
			(*t) = qRgba(cr, cg, cb, ca);
			s++;
			t++;
		}
	}

	createImageFrame(res, state, 3);

	delete imgStr;
	delete[] buffer;
	delete image;
	delete mskStr;
	delete[] mbuffer;
}

void SlaOutputDev::drawMaskedImage(GfxState *state, Object *ref, Stream *str,  int width, int height, GfxImageColorMap *colorMap, GBool interpolate, Stream *maskStr, int maskWidth, int maskHeight, GBool maskInvert, GBool maskInterpolate)
{
//	qDebug() << "SlaOutputDev::drawMaskedImage";
	ImageStream * imgStr = new ImageStream(str, width, colorMap->getNumPixelComps(), colorMap->getBits());
	imgStr->reset();
	unsigned int *dest = nullptr;
	unsigned char * buffer = new unsigned char[width * height * 4];
	QImage * image = nullptr;
	for (int y = 0; y < height; y++)
	{
		dest = (unsigned int *)(buffer + y * 4 * width);
		Guchar * pix = imgStr->getLine();
		colorMap->getRGBLine(pix, dest, width);
	}
	image = new QImage(buffer, width, height, QImage::Format_RGB32);
	if (image == nullptr || image->isNull())
	{
		delete imgStr;
		delete[] buffer;
		delete image;
		return;
	}
	ImageStream *mskStr = new ImageStream(maskStr, maskWidth, 1, 1);
	mskStr->reset();
	Guchar *mdest = nullptr;
	int invert_bit = maskInvert ? 1 : 0;
	unsigned char * mbuffer = new unsigned char[maskWidth * maskHeight];
	memset(mbuffer, 0, maskWidth * maskHeight);
	for (int y = 0; y < maskHeight; y++)
	{
		mdest = (Guchar *)(mbuffer + y * maskWidth);
		Guchar * pix = mskStr->getLine();
		for (int x = 0; x < maskWidth; x++)
		{
			if (pix[x] ^ invert_bit)
				*mdest++ = 0;
			else
				*mdest++ = 255;
		}
	}
	if ((maskWidth != width) || (maskHeight != height))
		*image = image->scaled(maskWidth, maskHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	QImage res = image->convertToFormat(QImage::Format_ARGB32);
	unsigned char cc, cm, cy, ck;
	int s = 0;
	for (int yi=0; yi < res.height(); ++yi)
	{
		QRgb *t = (QRgb*)(res.scanLine( yi ));
		for (int xi=0; xi < res.width(); ++xi)
		{
			cc = qRed(*t);
			cm = qGreen(*t);
			cy = qBlue(*t);
			ck = mbuffer[s];
			(*t) = qRgba(cc, cm, cy, ck);
			s++;
			t++;
		}
	}

	createImageFrame(res, state, colorMap->getNumPixelComps());

	delete imgStr;
	delete[] buffer;
	delete image;
	delete mskStr;
	delete[] mbuffer;
}

void SlaOutputDev::drawImage(GfxState *state, Object *ref, Stream *str, int width, int height, GfxImageColorMap *colorMap, GBool interpolate, POPPLER_CONST_082 int* maskColors, GBool inlineImg)
{
	ImageStream * imgStr = new ImageStream(str, width, colorMap->getNumPixelComps(), colorMap->getBits());
//	qDebug() << "SlaOutputDev::drawImage Image Components" << colorMap->getNumPixelComps() << "Mask" << maskColors;
	imgStr->reset();
	QImage* image = nullptr;
	if (maskColors)
	{
		image = new QImage(width, height, QImage::Format_ARGB32);
		for (int y = 0; y < height; y++)
		{
			QRgb *s = (QRgb*)(image->scanLine(y));
			Guchar *pix = imgStr->getLine();
			for (int x = 0; x < width; x++)
			{
				GfxRGB rgb;
				colorMap->getRGB(pix, &rgb);
				int Rc = qRound(colToDbl(rgb.r) * 255);
				int Gc = qRound(colToDbl(rgb.g) * 255);
				int Bc = qRound(colToDbl(rgb.b) * 255);
				*s = qRgba(Rc, Gc, Bc, 255);
				for (int i = 0; i < colorMap->getNumPixelComps(); ++i)
				{
					if (pix[i] < maskColors[2*i] * 255 || pix[i] > maskColors[2*i+1] * 255)
					{
						*s = *s | 0xff000000;
						break;
					}
				}
				s++;
				pix += colorMap->getNumPixelComps();
			}
		}
	}
	else
	{
		image = new QImage(width, height, QImage::Format_ARGB32);
		for (int y = 0; y < height; y++)
		{
			QRgb *s = (QRgb*)(image->scanLine(y));
			Guchar *pix = imgStr->getLine();
			for (int x = 0; x < width; x++)
			{
				if (colorMap->getNumPixelComps() == 4)
				{
					GfxCMYK cmyk;
					colorMap->getCMYK(pix, &cmyk);
					int Cc = qRound(colToDbl(cmyk.c) * 255);
					int Mc = qRound(colToDbl(cmyk.m) * 255);
					int Yc = qRound(colToDbl(cmyk.y) * 255);
					int Kc = qRound(colToDbl(cmyk.k) * 255);
					*s = qRgba(Yc, Mc, Cc, Kc);
				}
				else
				{
					GfxRGB rgb;
					colorMap->getRGB(pix, &rgb);
					int Rc = qRound(colToDbl(rgb.r) * 255);
					int Gc = qRound(colToDbl(rgb.g) * 255);
					int Bc = qRound(colToDbl(rgb.b) * 255);
					*s = qRgba(Rc, Gc, Bc, 255);
				}
				s++;
				pix += colorMap->getNumPixelComps();
			}
		}
	}

	if (image != nullptr && !image->isNull()) {
		createImageFrame(*image, state, colorMap->getNumPixelComps());
	}

	delete imgStr;
	delete image;
}

void SlaOutputDev::createImageFrame(QImage& image, GfxState *state, int numColorComponents)
{
//	qDebug() << "SlaOutputDev::createImageFrame";
	const double *ctm = state->getCTM();
	double xCoor = m_doc->currentPage()->xOffset();
	double yCoor = m_doc->currentPage()->yOffset();

	m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
	double angle = m_ctm.map(QLineF(0, 0, 1, 0)).angle();
	QPointF torigin;
	// In PDF all images considered squares with unit length that are transformed into the proper
	// dimensions by ctm.
	// A positive determinant retains orientation. Thus orientation is the same as in the PDF
	// coordinate system (y-axis increases upwards). As Scribus uses the inverse orientation the
	// image needs to be flipped (a horizontal flip is applied later).  For a flipped image the
	// corner that will be origin in Scribus is the upper right corner (1, 1) of the image.
	// A negative determinant changes the orientation such that the image is already in the Scribus
	// coordinate orientation and no flip is necessary. The origin will be the upper left corner (0, 1).
	if (m_ctm.determinant() > 0) {
		torigin = m_ctm.map(QPointF(1, 1));
	} else {
		torigin = m_ctm.map(QPointF(0, 1));
	}

	// Determine the visible area of the picture after clipping it. If it is empty, no item
	// needs to be created.
	QPainterPath outline;
	outline.addRect(0, 0, 1, 1);
	outline = m_ctm.map(outline);
	outline = intersection(outline, m_currentClipPath);

	if ((inPattern == 0) && (outline.isEmpty() || outline.boundingRect().isNull()))
		return;

    // Determine the width and height of the image by undoing the rotation part
	// of the CTM and applying the result to the unit square.
	QTransform without_rotation;
	without_rotation = m_ctm * without_rotation.rotate(angle);
	QRectF trect_wr = without_rotation.mapRect(QRectF(0, 0, 1, 1));

	int z = m_doc->itemAdd(PageItem::ImageFrame, PageItem::Rectangle, xCoor + torigin.x(), yCoor + torigin.y(), trect_wr.width(), trect_wr.height(), 0, CommonStrings::None, CommonStrings::None);
	PageItem* ite = m_doc->Items->at(z);
	ite->ClipEdited = true;
	ite->FrameType = 3;
	m_doc->setRedrawBounding(ite);
	ite->Clip = flattenPath(ite->PoLine, ite->Segments);
	ite->setTextFlowMode(PageItem::TextFlowDisabled);
	ite->setFillShade(100);
	ite->setLineShade(100);
	ite->setFillEvenOdd(false);
	ite->setFillTransparency(1.0 - state->getFillOpacity());
	ite->setFillBlendmode(getBlendMode(state));
	if (m_ctm.determinant() > 0)
	{
		ite->setRotation(-(angle - 180));
		ite->setImageFlippedH(true);
	}
	else
		ite->setRotation(-angle);
	m_doc->adjustItemSize(ite);

	if (numColorComponents == 4)
	{
		QTemporaryFile *tempFile = new QTemporaryFile(QDir::tempPath() + "/scribus_temp_pdf_XXXXXX.tif");
		tempFile->setAutoRemove(false);
		if (tempFile->open())
		{
			QString fileName = getLongPathName(tempFile->fileName());
			if (!fileName.isEmpty())
			{
				tempFile->close();
				ite->isInlineImage = true;
				ite->isTempFile = true;
				ite->AspectRatio = false;
				ite->ScaleType   = false;
				TIFF* tif = TIFFOpen(fileName.toLocal8Bit().data(), "w");
				if (tif)
				{
					TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, image.width());
					TIFFSetField(tif, TIFFTAG_IMAGELENGTH, image.height());
					TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
					TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
					TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
					TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
					TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
					for (int y = 0; y < image.height(); ++y)
					{
						TIFFWriteScanline(tif, image.scanLine(y), y);
					}
					TIFFClose(tif);
					m_doc->loadPict(fileName, ite);
				}
				m_Elements->append(ite);
				if (m_groupStack.count() != 0)
				{
					m_groupStack.top().Items.append(ite);
					applyMask(ite);
				}
			}
			else
				m_doc->Items->removeAll(ite);
		}
		delete tempFile;
	}
	else
	{
		QTemporaryFile *tempFile = new QTemporaryFile(QDir::tempPath() + "/scribus_temp_pdf_XXXXXX.png");
		tempFile->setAutoRemove(false);
		if (tempFile->open())
		{
			QString fileName = getLongPathName(tempFile->fileName());
			if (!fileName.isEmpty())
			{
				tempFile->close();
				ite->isInlineImage = true;
				ite->isTempFile = true;
				ite->AspectRatio = false;
				ite->ScaleType   = false;
				image.save(fileName, "PNG");
				m_doc->loadPict(fileName, ite);
				m_Elements->append(ite);
				if (m_groupStack.count() != 0)
				{
					m_groupStack.top().Items.append(ite);
					applyMask(ite);
				}
			}
			else
				m_doc->Items->removeAll(ite);
		}
		delete tempFile;
	}
	if (inPattern == 0)
	{
		outline.translate(xCoor - ite->xPos(), yCoor - ite->yPos());
		// Undo the rotation of the clipping path as it is rotated together with the iamge.
		QTransform mm;
		mm.rotate(-ite->rotation());
		outline = mm.map(outline);
		ite->PoLine.fromQPainterPath(outline, true);
		ite->setFillEvenOdd(outline.fillRule() == Qt::OddEvenFill);
		ite->ClipEdited = true;
		ite->FrameType = 3;
		ite->setTextFlowMode(PageItem::TextFlowDisabled);
		ite->ScaleType   = true;
		m_doc->adjustItemSize(ite);
		ite->OldB2 = ite->width();
		ite->OldH2 = ite->height();
		ite->updateClip();
	}
}

void SlaOutputDev::beginMarkedContent(POPPLER_CONST char *name, Object *dictRef)
{
	mContent mSte;
	mSte.name = QString(name);
	mSte.ocgName = "";
	if (importerFlags & LoadSavePlugin::lfCreateDoc)
	{
		if (dictRef->isNull())
			return;
		Object dictObj;
		Dict *dict;
		Object dictType;
		OCGs *contentConfig = catalog->getOptContentConfig();
		OptionalContentGroup *oc;
		if (dictRef->isRef())
		{
			oc = contentConfig->findOcgByRef(dictRef->getRef());
			if (oc)
			{
//				qDebug() << "Begin OCG Content (Ref) with Name " << QString(name) << "Layer" << UnicodeParsedString(oc->getName());
				m_doc->setActiveLayer(UnicodeParsedString(oc->getName()));
				mSte.ocgName = UnicodeParsedString(oc->getName());
			}
		}
		else
		{
			dictObj = dictRef->fetch(xref);
			if (!dictObj.isDict())
				return;
			dict = dictObj.getDict();
			dictType = dict->lookup("Type");
			if (dictType.isName("OCG"))
			{
				oc = contentConfig->findOcgByRef(dictRef->getRef());
				if (oc)
				{
					//					qDebug() << "Begin OCG Content with Name " << UnicodeParsedString(oc->getName());
					m_doc->setActiveLayer(UnicodeParsedString(oc->getName()));
					mSte.ocgName = UnicodeParsedString(oc->getName());
				}
			}
		}
	}
	m_mcStack.push(mSte);
}

void SlaOutputDev::beginMarkedContent(POPPLER_CONST char *name, Dict *properties)
{
//	qDebug() << "Begin Marked Content with Name " << QString(name);
	QString nam = QString(name);
	mContent mSte;
	mSte.name = nam;
	mSte.ocgName = "";
	m_mcStack.push(mSte);
	if (importerFlags & LoadSavePlugin::lfCreateDoc)
	{
		if (nam == "Layer")		// Handle Adobe Illustrator Layer command
		{
			if (layersSetByOCG)
				return;
			QString lName = QString("Layer_%1").arg(layerNum + 1);
			Object obj = properties->lookup((char*) "Title");
			if (obj.isString())
				lName = QString(obj.getString()->getCString());
			for (ScLayers::iterator it = m_doc->Layers.begin(); it != m_doc->Layers.end(); ++it)
			{
				if (it->Name == lName)
				{
					m_doc->setActiveLayer(lName);
					return;
				}
			}
			layerNum++;
			if (!firstLayer)
				currentLayer = m_doc->addLayer(lName, true);
			firstLayer = false;

			obj = properties->lookup((char*) "Visible");
			if (obj.isBool())
				m_doc->setLayerVisible(currentLayer, obj.getBool());
			obj = properties->lookup((char*) "Editable");
			if (obj.isBool())
				m_doc->setLayerLocked(currentLayer, !obj.getBool());
			obj = properties->lookup((char*) "Printed");
			if (obj.isBool())
				m_doc->setLayerPrintable(currentLayer, obj.getBool());
			obj = properties->lookup((char*)"Color");
			if (obj.isArray())
			{
				Object obj1;
				obj1 = obj.arrayGet(0);
				int r = obj1.getNum() / 256;
				obj1 = obj.arrayGet(1);
				int g = obj1.getNum() / 256;
				obj1 = obj.arrayGet(2);
				int b = obj1.getNum() / 256;
				m_doc->setLayerMarker(currentLayer, QColor(r, g, b));
			}
		}
	}
}

void SlaOutputDev::endMarkedContent(GfxState *state)
{
//	qDebug() << "End Marked Content";
	if (m_mcStack.count() > 0)
	{
		mContent mSte = m_mcStack.pop();
		if (importerFlags & LoadSavePlugin::lfCreateDoc)
		{
			if (mSte.name == "OC")
			{
				for (ScLayers::iterator it = m_doc->Layers.begin(); it != m_doc->Layers.end(); ++it)
				{
					if (it->Name == mSte.ocgName)
					{
						m_doc->setActiveLayer(mSte.ocgName);
						return;
					}
				}
			}
		}
	}
}

void SlaOutputDev::markPoint(POPPLER_CONST char *name)
{
//	qDebug() << "Begin Marked Point with Name " << QString(name);
}

void SlaOutputDev::markPoint(POPPLER_CONST char *name, Dict *properties)
{
//	qDebug() << "Begin Marked Point with Name " << QString(name) << "and Properties";
	beginMarkedContent(name, properties);
}


/*************************************************
importing text as text
*************************************************/

/**
 * \brief Sets _invalidated_style to true to indicate that styles have to be updated
 * Used for text output when glyphs are buffered till a font change
 * TODO: there are a whole bunch of functions to override that need to call this so that a font change is recognised and text flushed.
 */
void SlaOutputDev::_updateStyle(GfxState *state) {
	qDebug() << "_updateStyle()";
    if (_in_text_object) {
        _invalidated_style = true;
    }
}

/*
    MatchingChars
    Count for how many characters s1 matches sp taking into account
    that a space in sp may be removed or replaced by some other tokens
    specified in the code. (Bug LP #179589)
*/
static size_t MatchingChars(QString s1, QString sp)
{
	qDebug() << "MatchingChars()";
	//use uinit not size_t to keep QT happy
	uint is = 0;
	uint ip = 0;

    while(is < s1.length() && ip < sp.length()) {
        if (s1[is] == sp[ip]) {
            is++; ip++;
        } else if (sp[ip] == ' ') {
            ip++;
            if (s1[is] == '_') { // Valid matches to spaces in sp.
                is++;
            }
        } else {
            break;
        }
    }
    return ip;
}

/*
    SvgBuilder::_BestMatchingFont
    Scan the available fonts to find the font name that best matches PDFname.
    (Bug LP #179589)
*/
QString SlaOutputDev::_bestMatchingFont(QString PDFname)
{
	qDebug() << "_bestMatchingFont()";
	double bestMatch = 0;
    QString bestFontname = "Arial";

    for (auto fontname : _availableFontNames) {
        // At least the first word of the font name should match.
        size_t minMatch = fontname.indexOf(" ");
        if (minMatch == std::string::npos) {
           minMatch = fontname.length();
        }

        size_t Match = MatchingChars(PDFname, fontname);
        if (Match >= minMatch) {
            double relMatch = (float)Match / (fontname.length() + PDFname.length());
            if (relMatch > bestMatch) {
                bestMatch = relMatch;
                bestFontname = fontname;
            }
        }
    }

    if (bestMatch == 0)
        return PDFname;
    else
        return bestFontname;
}

/**
 * This array holds info about translating font weight names to more or less CSS equivalents
 */
static char *font_weight_translator[][2] = {
    {(char*) "bold",        (char*) "bold"},
    {(char*) "light",       (char*) "300"},
    {(char*) "black",       (char*) "900"},
    {(char*) "heavy",       (char*) "900"},
    {(char*) "ultrabold",   (char*) "800"},
    {(char*) "extrabold",   (char*) "800"},
    {(char*) "demibold",    (char*) "600"},
    {(char*) "semibold",    (char*) "600"},
    {(char*) "medium",      (char*) "500"},
    {(char*) "book",        (char*) "normal"},
    {(char*) "regular",     (char*) "normal"},
    {(char*) "roman",       (char*) "normal"},
    {(char*) "normal",      (char*) "normal"},
    {(char*) "ultralight",  (char*) "200"},
    {(char*) "extralight",  (char*) "200"},
    {(char*) "thin",        (char*) "100"}
};

void SlaOutputDev::updateFont(GfxState* state) {
	qDebug() << "updateFont()";
	if (this->import_text_as_vectors)
	{
		_updateFontForVectors(state);
	}
	else {
		_updateFontForText(state);
	}
}
/**
 * \brief Updates _font_style according to the font set in parameter state
 */
void SlaOutputDev::_updateFontForText(GfxState *state) {

	qDebug() << "updateFontForText()";
    _need_font_update = false;
	updateTextMat(state);    // Ensure that we have a text matrix built

    //if (_font_style) {
        //sp_repr_css_attr_unref(_font_style);
    //}
	// TODO: Found out if the fontstyle needs to be set to blank or if this function sets all the prolperties anyway so blanking it makes no difference
	_font_style;// = new ScFace();
    GfxFont *font = state->getFont();
    // Store original name
	QString _font_specification;
    if (font != NULL && font->getName()) {
        _font_specification = font->getName()->getCString();
    } else {		
        _font_specification = "Arial";
    }

    // Prune the font name to get the correct font family name
    // In a PDF font names can look like this: IONIPB+MetaPlusBold-Italic
    QString font_family;
    QString font_style;
	QString font_style_lowercase;
    QString plus_sign = _font_specification.mid(_font_specification.indexOf("+") + 1);
    if (plus_sign.length() > 0) {
        font_family = QString(plus_sign + 1);		
        _font_specification = plus_sign + 1;
    } else {
        font_family = _font_specification;
    }
	

    size_t style_delim = 0;
    if ( ((style_delim = font_family.indexOf("-")) >= 0 ) ||
         ((style_delim = font_family.indexOf(",") >= 0))) {
		font_style = font_family.mid(style_delim);
		font_style_lowercase = font_style.toLower();
    }

	QString cs_font_family = "Arial";
    // Font family
    if (font != NULL && font->getFamily()) { // if font family is explicitly given use it.
		cs_font_family = font->getFamily()->getCString();
    } else {
        int attr_value = 1;
        //sp_repr_get_int(_preferences, "localFonts", &attr_value);
		//TODO: Implement this
        if (attr_value != 0) {
            // Find the font that best matches the stripped down (orig)name (Bug LP #179589).
			cs_font_family =  _bestMatchingFont(font_family);
        } else {
			cs_font_family = font_family;
        }
    }

	_font_style.font.setFamily(cs_font_family);
	
	_font_style.font.setStyle(QFont::StyleNormal);
    // Font style
    if (font != NULL && font->isItalic()) {
		_font_style.font.setStyle(QFont::StyleItalic);
    } else if (font_style != "") {
        if ( (font_style_lowercase.indexOf("italic") >= 0) ||
			font_style_lowercase.indexOf("slanted") >= 0 ) {
			_font_style.font.setStyle(QFont::StyleItalic);
        } else if (font_style_lowercase.indexOf("oblique") >= 0) {
			_font_style.font.setStyle(QFont::StyleOblique);
        }
    }
	
	
    // Font variant -- default 'normal' value
	//cs_font_style = "normal";

    // Font weight
	GfxFont::Weight font_weight = font != NULL ? font->getWeight() : GfxFont::W400;
    char *css_font_weight = nullptr;
    if ( font_weight != GfxFont::WeightNotDefined ) {
        if ( font_weight == GfxFont::W400 ) {
			_font_style.font.setWeight(QFont::Normal);
        } else if ( font_weight == GfxFont::W700 ) {
			_font_style.font.setWeight(QFont::Bold);
        } else {
			//TODO: Implement this, it's not obvious what to iplement without bei8ng able to see the pdf data
            //gchar weight_num[4] = "100";
            //weight_num[0] = (gchar)( '1' + (font_weight - GfxFont::W100) );
			//cs_font_style = (gchar *)&weight_num;
			_font_style.font.setWeight(QFont::Normal);
        }
    } else if (font_style != "") {
        // Apply the font weight translations
		/* TODO: Implement this, it's somewhat non-obvious without the data
        int num_translations = sizeof(font_weight_translator) / ( 2 * sizeof(char *) );
        for ( int i = 0 ; i < num_translations ; i++ ) {
            if (strstr(font_style_lowercase, font_weight_translator[i][0])) {
                css_font_weight = font_weight_translator[i][1];
            }
        }
		*/
		_font_style.font.setWeight(QFont::Normal);
    } else {
		_font_style.font.setWeight(QFont::Normal);
    }

	_font_style.font.setStretch(QFont::Unstretched);
	//TODO: this may be the correct default _font_style.font.setStretch(QFont::AnyStretch);

    // Font stretch
	GfxFont::Stretch font_stretch = font != NULL ? font->getStretch() : GfxFont::Normal;
    switch (font_stretch) {
        case GfxFont::UltraCondensed:
			_font_style.font.setStretch(QFont::UltraCondensed);
            break;
        case GfxFont::ExtraCondensed:
			_font_style.font.setStretch(QFont::ExtraCondensed);            
            break;
        case GfxFont::Condensed:
			_font_style.font.setStretch(QFont::Condensed);            
            break;
        case GfxFont::SemiCondensed:
			_font_style.font.setStretch(QFont::SemiCondensed);
            break;
        case GfxFont::Normal:
			_font_style.font.setStretch(QFont::Unstretched);            
            break;
        case GfxFont::SemiExpanded:
			_font_style.font.setStretch(QFont::SemiExpanded);            
            break;
        case GfxFont::Expanded:
			_font_style.font.setStretch(QFont::Expanded);            
            break;
        case GfxFont::ExtraExpanded:
			_font_style.font.setStretch(QFont::ExtraExpanded);            
            break;
        case GfxFont::UltraExpanded:
			_font_style.font.setStretch(QFont::UltraExpanded);           
            break;
        default:
            break;
    }


    // Font size
    //Inkscape::CSSOStringStream os_font_size;
    double css_font_size = _font_scaling * state->getFontSize();
	if (font != 0) {
		if (font->getType() == fontType3) {
			const double* font_matrix = font->getFontMatrix();
			if (font_matrix[0] != 0.0) {
				css_font_size *= font_matrix[3] / font_matrix[0];
			}
		}
	}
	_font_style.font.setPointSizeF(css_font_size);

	/* This doesn't appear to be supported by QT
    // Writing mode
    if ( font->getWMode() == 0 ) {
		_font_style.font.setW
        sp_repr_css_set_property(_font_style, "writing-mode", "lr");
    } else {
        sp_repr_css_set_property(_font_style, "writing-mode", "tb");
    }
	*/
	//I'm guessing that this looks up the font after all the parameters hav ebeen set.
	_font_style.font.resolve();

    _current_font = _font_style.font;
    _invalidated_style = true;
}

/**
 * \brief Shifts the current text position by the given amount (specified in text space)
 */
void SlaOutputDev::updateTextShift(GfxState *state, double shift) {
	qDebug() << "updateTextShift()";
    double shift_value = -shift * 0.001 * fabs(state->getFontSize());
    if (state->getFont()->getWMode()) {
        _text_position.setY(_text_position.y() + shift_value);
    } else {
        _text_position.setX(_text_position.x() + shift_value);
    }
}

/**
 * \brief Updates current text position
 */
void SlaOutputDev::updateTextPos(GfxState* state) {
	qDebug() << "updateTextPos()";
    QPoint new_position = QPoint(state->getCurX(), state->getCurY());
    _text_position = new_position;
}

/**
 * \brief Flushes the buffered characters
 */
void SlaOutputDev::updateTextMat(GfxState *state) {
	qDebug() << "updateTextMat()";
    _flushText(state);
    // Update text matrix
    const double *text_matrix = state->getTextMat();
    double w_scale = sqrt( text_matrix[0] * text_matrix[0] + text_matrix[2] * text_matrix[2] );
    double h_scale = sqrt( text_matrix[1] * text_matrix[1] + text_matrix[3] * text_matrix[3] );
    double max_scale;
    if ( w_scale > h_scale ) {
        max_scale = w_scale;
    } else {
        max_scale = h_scale;
    }
    // Calculate new text matrix
	QTransform new_text_matrix =  QTransform(text_matrix[0] * state->getHorizScaling(),
                               text_matrix[1] * state->getHorizScaling(),
                               -text_matrix[2], -text_matrix[3],
                               0.0, 0.0);

    if (!qFuzzyCompare(fabs( max_scale), 1.0))  {
        // Cancel out scaling by font size in text matrix
        for ( int i = 0 ; i < 4 ; i++ ) {
            new_text_matrix.scale(1.0 / max_scale, 1.0 / max_scale); //this may need to be byx or by y
        }
    }
    _text_matrix = new_text_matrix;
    _font_scaling = max_scale;
}

/**
 * \brief Writes the buffered characters to the SVG document
 */
void SlaOutputDev::_flushText(GfxState *state) {
	qDebug() << "_flushText()";
    // Ignore empty strings
    if ( _glyphs.empty()) {
        _glyphs.clear();
        return;
    }
    std::vector<PdfGlyph>::iterator i = _glyphs.begin();
    const PdfGlyph& first_glyph = (*i);
    int render_mode = first_glyph.render_mode;
    // Ignore invisible characters
    if ( render_mode == 3 ) {
        _glyphs.clear();
        return;
    }

	PageItem* text_node;


	
	//QString textColor = importColor(first_glyph.);
	FPointArray textPath;
	
		double xCoor = m_doc->currentPage()->xOffset();
		double yCoor = m_doc->currentPage()->yOffset();
		double  lineWidth = 0.0;		
		int z = m_doc->itemAdd(PageItem::TextFrame, PageItem::Rectangle, xCoor, yCoor, 10, 10, 0, CommonStrings::None, CommonStrings::None);
		//int z = m_doc->itemAdd(PageItem::Polygon, PageItem::Unspecified, xCoor, yCoor, 10, 10, 0, CommonStrings::None, CommonStrings::None);


		//int z = m_doc->itemAdd(PageItem::Polygon, PageItem::Unspecified, xCoor, yCoor, 10, 10, 0, CommonStrings::None, CommonStrings::None);
		PageItem* ite = m_doc->Items->at(z);
		QTransform mm;
		mm.scale(1, -1);
		mm.translate(_text_matrix.m32(), -_text_matrix.m33());
		textPath.map(mm);
		textPath.map(m_ctm);
		ite->PoLine = textPath.copy();

		ite->ClipEdited = true;
		ite->FrameType = 3;
		ite->setLineEnd(PLineEnd);
		ite->setLineJoin(PLineJoin);
		ite->setTextFlowMode(PageItem::TextFlowDisabled);
		ite->OwnPage = m_doc->OnPage(ite);

		int textRenderingMode = state->getRender();
		// Invisible or only used for clipping
		if (textRenderingMode == 3)
			return;
		// Fill text rendering modes. See above
		if (textRenderingMode == 0 || textRenderingMode == 2 || textRenderingMode == 4 || textRenderingMode == 6)
		{
			CurrColorFill = getColor(state->getFillColorSpace(), state->getFillColor(), &CurrFillShade);
			ite->setFillColor(CurrColorFill);
			ite->setFillShade(CurrFillShade);
			ite->setFillEvenOdd(false);
			ite->setFillTransparency(1.0 - state->getFillOpacity());
			ite->setFillBlendmode(getBlendMode(state));
		}
		// Stroke text rendering modes. See above
		if (textRenderingMode == 1 || textRenderingMode == 2 || textRenderingMode == 5 || textRenderingMode == 6)
		{
			CurrColorStroke = getColor(state->getStrokeColorSpace(), state->getStrokeColor(), &CurrStrokeShade);
			ite->setLineColor(CurrColorStroke);
			ite->setLineWidth(state->getTransformedLineWidth());
			ite->setLineTransparency(1.0 - state->getStrokeOpacity());
			ite->setLineBlendmode(getBlendMode(state));
			ite->setLineShade(CurrStrokeShade);
		}
		/*
		m_doc->adjustItemSize(ite);
		m_Elements->append(ite);
		if (m_groupStack.count() != 0)
		{
			m_groupStack.top().Items.append(ite);
			applyMask(ite);
		}

		*/
		/*****************************/


		text_node = m_doc->Items->at(z);

		m_doc->adjustItemSize(ite);
		m_Elements->append(ite);

    // Set text matrix... This need to be done so that the globaal world view that we rite out glyphs to is transformed correctly by the context matrix for each glyph, possibly anyhow.
    QTransform text_transform(_text_matrix);
    text_transform.setMatrix(text_transform.m11(), text_transform.m12(),0,
		text_transform.m21(), text_transform.m22(), 0,
		first_glyph.position.x(),  first_glyph.position.y(), 1);
	/* todo, set the global transform
    gchar *transform = sp_svg_transform_write(text_transform);
    text_node->setAttribute("transform", transform);
    g_free(transform);
	*/
    bool new_tspan = true;
    bool same_coords[2] = {true, true};
    QPoint last_delta_pos;
    unsigned int glyphs_in_a_row = 0;
	//TODO: Verify this, but I think we just use text_node for the main bodyu then use a painterPath for tspan_node, but only create it once all the text is rfeady to write.



    //Inkscape::XML::Node *tspan_node = nullptr;
	QVector<double> x_coords;
	QVector<double> y_coords;
    QString text_buffer;
	bool btspan_node = false;	
    // Output all buffered glyphs
	i = _glyphs.begin();
	while (true) {
		const PdfGlyph& glyph = (*i);
		qDebug() << "PdfGlyph: '" << glyph.code << "'";

		// Append the character to the text buffer, I thinkl this should be at the beging otherwiose you miss off the last character.
		if (glyph.code.length() != 0 && !glyph.code.isNull()) { //don't know which one is equivalent to empty
			text_buffer += glyph.code;
		}

		std::vector<PdfGlyph>::iterator prev_iterator;// = NULL;
		if (i != _glyphs.begin()) {
			prev_iterator = i - 1;
		}
        // Check if we need to make a new tspan
        if (glyph.style_changed) {
            new_tspan = true;
        } else if ( i != _glyphs.begin() ) {
            const PdfGlyph& prev_glyph = (*prev_iterator);
            if ( !( ( glyph.dy == 0.0 && prev_glyph.dy == 0.0 &&
                     glyph.text_position.y() == prev_glyph.text_position.y() ) ||
                    ( glyph.dx == 0.0 && prev_glyph.dx == 0.0 &&
                     glyph.text_position.x() == prev_glyph.text_position.x() ) ) ) {
                new_tspan = true;
            }
        }

        // Create tspan node if needed
        if ( new_tspan || i == _glyphs.end() - 1) {
            if (btspan_node) {
                // Set the x and y coordinate arrays
	


				/*
                if ( same_coords[0] ) {
                    sp_repr_set_svg_double(tspan_node, "x", last_delta_pos[0]);
                } else {
                    tspan_node->("x", x_coords);
                }
				
                if ( same_coords[1] ) {
                    sp_repr_set_svg_double(tspan_node, "y", last_delta_pos[1]);
                } else {
                    tspan_node->setAttributeOrRemoveIfEmpty("y", y_coords);
                }
				*/
				double startX = x_coords[0];
				double startY = y_coords[0];
				qDebug() << "tspan content: " <<  text_buffer;
				ite->itemText.insertChars(text_buffer);
				ite->itemText.trim();
				ite->update(); //don't knoww which one if either i need to get it to redraw.
				ite->updateClip();
				/*
                if ( glyphs_in_a_row > 1 ) {
                    tspan_node->setAttribute("sodipodi:role", "line");
                }
				*/
                // Add text content node to tspan
                //Inkscape::XML::Node *text_content = _xml_doc->createTextNode(text_buffer.c_str());	
				QPainterPath painterPath;
				painterPath.addText(startX, startY, glyph.style->getFont(), text_buffer);
				if (!textPath.empty())
				{					
					//text_node.append(text_node);
					text_node->PoLine.append(textPath);
				}
                // Clear temporary buffers
                x_coords.clear();
                y_coords.clear();
                text_buffer.clear();
                //Inkscape::GC::release(tspan_node);
                glyphs_in_a_row = 0;
            }
			if (i == _glyphs.end() - 1) {
				//sp_repr_css_attr_unref((*prev_iterator).style);
				break;
            } else {
				btspan_node = true;
                //tspan_node = _xml_doc->createElement("svg:tspan");

                ///////
                // Create a font specification string and save the attribute in the style
                //PangoFontDescription *descr = pango_font_description_from_string(glyph.font_specification);
                //Glib::ustring properFontSpec = font_factory::Default()->ConstructFontSpecification(descr);
                //pango_font_description_free(descr);
				
				glyph.style->setFont(this->_current_font);

                // Set style and unref SPCSSAttr if it won't be needed anymore
                // assume all <tspan> nodes in a <text> node share the same style
                if ( glyph.style_changed && i != _glyphs.begin() ) {    // Free previous style
					//TODO: Tidy up, makle sure we have no leaks.
                    //sp_repr_css_attr_unref((*prev_iterator).style);
                }
            }
            new_tspan = false;
        }
        if ( glyphs_in_a_row > 0 ) {
            //x_coords.append(" ");
            //y_coords.append(" ");
			same_coords[0] = true;
			same_coords[1] = true;
            // Check if we have the same coordinates
            const PdfGlyph& prev_glyph = (*prev_iterator);
            if ( glyph.text_position.x() != prev_glyph.text_position.x() ) {
                same_coords[0] = false;
            }
			if (glyph.text_position.y() != prev_glyph.text_position.y()) {
				same_coords[1] = false;
			}
        }
        // Append the coordinates to their respective strings
        QPoint delta_pos( glyph.text_position - first_glyph.text_position );
		delta_pos.setY(delta_pos.y() + glyph.rise);
		delta_pos.setY(delta_pos.y() * -1.0); //flip-it
		delta_pos.setY(delta_pos.y() * _font_scaling);
        x_coords.append(delta_pos.x());
        y_coords.append(delta_pos.y());
        last_delta_pos = delta_pos;

        glyphs_in_a_row++;
        ++i;
    }
    _glyphs.clear();

	if (m_groupStack.count() != 0)
	{
		m_groupStack.top().Items.append(ite);
		applyMask(ite);
	}

}


/**
 * \brief Adds the specified character to the text buffer
 * Takes care of converting it to UTF-8 and generates a new style repr if style
 * has changed since the last call.
 */
void SlaOutputDev::addChar(GfxState *state, double x, double y,
                         double dx, double dy,
                         double originX, double originY,
                         CharCode /*code*/, int /*nBytes*/, Unicode const *u, int uLen) {
	qDebug() << "addChar() '" << u <<" : " << uLen;


    bool is_space = ( uLen == 1 && u[0] == 32 );
    // Skip beginning space
    if ( is_space && _glyphs.empty()) {
        QPoint delta(dx, dy);
         _text_position += delta;
         return;
    }
    // Allow only one space in a row
    if ( is_space && /* _glyphs[_glyphs.size() - 1].code.size() == 1) && */
         (_glyphs[_glyphs.size() - 1].code == QChar::SpecialCharacter::Space) ) {
        QPoint delta(dx, dy);
        _text_position += delta;
        return;
    }

    PdfGlyph new_glyph;
    new_glyph.is_space = is_space;
    new_glyph.position = QPoint( x - originX, y - originY );
    new_glyph.text_position = _text_position;
    new_glyph.dx = dx;
    new_glyph.dy = dy;
    QPoint delta(dx, dy);
    _text_position += delta;

    // Convert the character to UTF-16 since that's our SVG document's encoding
    {
        //uchar uu[8] = {0};

        //for (int i = 0; i < uLen; i++) {
        //    uu[i] = u[i];
        //}

        //gchar *tmp = g_utf16_to_utf8(uu, uLen, nullptr, nullptr, nullptr);
        //if ( tmp && *tmp ) {
		//for (int i = 0; i < uLen; i++) {
		//	new_glyph.code += (char16_t)uu[i];
		//}		
		for (int i = 0; i < uLen; i++) {
			new_glyph.code += (char16_t)u[i];
		}        
        //} else {
        //   new_glyph.code.clear();
        //}
        //g_free(tmp);
    }

    // Copy current style if it has changed since the previous glyph
    if (_invalidated_style || _glyphs.empty()) {
        new_glyph.style_changed = true;
        int render_mode = state->getRender();
        // Set style
        bool has_fill = !( render_mode & 1 );
        bool has_stroke = ( render_mode & 3 ) == 1 || ( render_mode & 3 ) == 2;
		PdfGlyphStyle* glyph_style = new PdfGlyphStyle();
		glyph_style->setFill(has_fill);
		glyph_style->setStroke(has_stroke);
		glyph_style->setFont(this->_current_font); //or state->font, but i need the one i converted into a qfont
		new_glyph.style = glyph_style;
        // Find a way to handle blend modes on text
        /* GfxBlendMode blendmode = state->getBlendMode();
        if (blendmode) {
            sp_repr_css_set_property(new_glyph.style, "mix-blend-mode", enum_blend_mode[blendmode].key);
        } */
        new_glyph.render_mode = render_mode;
        //sp_repr_css_merge(new_glyph.style, _font_style); // Merge with font style
        _invalidated_style = false;
    } else {
        new_glyph.style_changed = false;
        // Point to previous glyph's style information
        const PdfGlyph& prev_glyph = _glyphs.back();
        new_glyph.style = prev_glyph.style;
        /* GfxBlendMode blendmode = state->getBlendMode();
        if (blendmode) {
            sp_repr_css_set_property(new_glyph.style, "mix-blend-mode", enum_blend_mode[blendmode].key);
        } */
        new_glyph.render_mode = prev_glyph.render_mode;
    }
	new_glyph.font_specification = this->_current_font.toString();// _font_specification;
    new_glyph.rise = state->getRise();

    _glyphs.push_back(new_glyph);
}

void SlaOutputDev::beginString(GfxState* state, const GooString* s)
{
	qDebug() << "beginString()" << s;
	if (_need_font_update) {
		_updateFontForText(state);
	}
	if (auto m = state->getTextMat()) {
		qDebug() << "tm:" << m[0] << "," << m[1] << "," << m[2] << "," << m[3] << "," << m[4] << "," << m[5];
	}
	if (auto m = state->getCTM()) {
		qDebug() << "ctm:" << m[0] << "," << m[1] << "," << m[2] << "," << m[3] << "," << m[4] << "," << m[5];
	}
}

void SlaOutputDev::endString(GfxState * /*state*/) {
	qDebug() << "endString()";
}




/*************************************************
importing text as glyphs
*************************************************/

void SlaOutputDev::_updateFontForVectors(GfxState *state)
{
	qDebug() << "_updateFontForVectors()";
	GfxFont *gfxFont;
	GfxFontLoc *fontLoc;
	GfxFontType fontType;
	SplashOutFontFileID *id;
	SplashFontFile *fontFile;
	SplashFontSrc *fontsrc = nullptr;
	FoFiTrueType *ff;
	Object refObj, strObj;
	GooString *fileName;
	char *tmpBuf;
	int tmpBufLen = 0;
	int *codeToGID;
	const double *textMat;
	double m11, m12, m21, m22, fontSize;
	SplashCoord mat[4];
	int n = 0;
	int faceIndex = 0;
	SplashCoord matrix[6];

	m_font = nullptr;
	fileName = nullptr;
	tmpBuf = nullptr;
	fontLoc = nullptr;

	if (!(gfxFont = state->getFont())) {
		goto err1;
	}
	fontType = gfxFont->getType();
	if (fontType == fontType3) {
		goto err1;
	}

	// check the font file cache
	id = new SplashOutFontFileID(gfxFont->getID());
	if ((fontFile = m_fontEngine->getFontFile(id)))
		delete id;
	else
	{
		if (!(fontLoc = gfxFont->locateFont(xref, nullptr)))
		{
			error(errSyntaxError, -1, "Couldn't find a font for '{0:s}'",
			gfxFont->getName() ? gfxFont->getName()->getCString() : "(unnamed)");
			goto err2;
		}

		// embedded font
		if (fontLoc->locType == gfxFontLocEmbedded)
		{
			// if there is an embedded font, read it to memory
			tmpBuf = gfxFont->readEmbFontFile(xref, &tmpBufLen);
			if (! tmpBuf)
				goto err2;

			// external font
		}
		else
		{ // gfxFontLocExternal
			fileName = fontLoc->path;
			fontType = fontLoc->fontType;
		}

		fontsrc = new SplashFontSrc;
		if (fileName)
			fontsrc->setFile(fileName, gFalse);
		else
			fontsrc->setBuf(tmpBuf, tmpBufLen, gTrue);

		// load the font file
		switch (fontType) {
		case fontType1:
			if (!(fontFile = m_fontEngine->loadType1Font(
				id,
				fontsrc,
				(const char **)((Gfx8BitFont *) gfxFont)->getEncoding())))
			{
				error(errSyntaxError, -1, "Couldn't create a font for '{0:s}'",
				gfxFont->getName() ? gfxFont->getName()->getCString() : "(unnamed)");
				goto err2;
			}
			break;
		case fontType1C:
			if (!(fontFile = m_fontEngine->loadType1CFont(
							id,
							fontsrc,
							(const char **)((Gfx8BitFont *) gfxFont)->getEncoding())))
			{
				error(errSyntaxError, -1, "Couldn't create a font for '{0:s}'",
				gfxFont->getName() ? gfxFont->getName()->getCString() : "(unnamed)");
				goto err2;
			}
			break;
		case fontType1COT:
			if (!(fontFile = m_fontEngine->loadOpenTypeT1CFont(
							id,
							fontsrc,
							(const char **)((Gfx8BitFont *) gfxFont)->getEncoding())))
			{
				error(errSyntaxError, -1, "Couldn't create a font for '{0:s}'",
				gfxFont->getName() ? gfxFont->getName()->getCString() : "(unnamed)");
				goto err2;
			}
			break;
		case fontTrueType:
		case fontTrueTypeOT:
			if (fileName)
				ff = FoFiTrueType::load(fileName->getCString());
			else
				ff = FoFiTrueType::make(tmpBuf, tmpBufLen);
			if (ff)
			{
				codeToGID = ((Gfx8BitFont *)gfxFont)->getCodeToGIDMap(ff);
				n = 256;
				delete ff;
			}
			else
			{
				codeToGID = nullptr;
				n = 0;
			}
			if (!(fontFile = m_fontEngine->loadTrueTypeFont(
							id,
							fontsrc,
							codeToGID, n)))
			{
				error(errSyntaxError, -1, "Couldn't create a font for '{0:s}'",
				gfxFont->getName() ? gfxFont->getName()->getCString() : "(unnamed)");
				goto err2;
			}
			break;
		case fontCIDType0:
		case fontCIDType0C:
			if (!(fontFile = m_fontEngine->loadCIDFont(
							id,
							fontsrc)))
			{
				error(errSyntaxError, -1, "Couldn't create a font for '{0:s}'",
				gfxFont->getName() ? gfxFont->getName()->getCString() : "(unnamed)");
				goto err2;
			}
			break;
		case fontCIDType0COT:
			if (((GfxCIDFont *) gfxFont)->getCIDToGID())
			{
				n = ((GfxCIDFont *) gfxFont)->getCIDToGIDLen();
				codeToGID = (int *) gmallocn(n, sizeof(*codeToGID));
				memcpy(codeToGID, ((GfxCIDFont *) gfxFont)->getCIDToGID(), n * sizeof(*codeToGID));
			}
			else
			{
				codeToGID = nullptr;
				n = 0;
			}
			if (!(fontFile = m_fontEngine->loadOpenTypeCFFFont(
							id,
							fontsrc,
							codeToGID, n)))
			{
				error(errSyntaxError, -1, "Couldn't create a font for '{0:s}'",
				gfxFont->getName() ? gfxFont->getName()->getCString() : "(unnamed)");
				goto err2;
			}
			break;
		case fontCIDType2:
		case fontCIDType2OT:
			codeToGID = nullptr;
			n = 0;
			if (((GfxCIDFont *) gfxFont)->getCIDToGID())
			{
				n = ((GfxCIDFont *) gfxFont)->getCIDToGIDLen();
				if (n)
				{
					codeToGID = (int *)gmallocn(n, sizeof(*codeToGID));
					memcpy(codeToGID, ((GfxCIDFont *)gfxFont)->getCIDToGID(), n * sizeof(*codeToGID));
				}
			}
			else
			{
				if (fileName)
					ff = FoFiTrueType::load(fileName->getCString());
				else
					ff = FoFiTrueType::make(tmpBuf, tmpBufLen);
				if (! ff)
					goto err2;
				codeToGID = ((GfxCIDFont *)gfxFont)->getCodeToGIDMap(ff, &n);
				delete ff;
			}
			if (!(fontFile = m_fontEngine->loadTrueTypeFont(
							id,
							fontsrc,
							codeToGID, n, faceIndex)))
			{
				error(errSyntaxError, -1, "Couldn't create a font for '{0:s}'",
				gfxFont->getName() ? gfxFont->getName()->getCString() : "(unnamed)");
				goto err2;
			}
			break;
		default:
			// this shouldn't happen
			goto err2;
		}
	}
	// get the font matrix
	textMat = state->getTextMat();
	fontSize = state->getFontSize();
	m11 = textMat[0] * fontSize * state->getHorizScaling();
	m12 = textMat[1] * fontSize * state->getHorizScaling();
	m21 = textMat[2] * fontSize;
	m22 = textMat[3] * fontSize;
	matrix[0] = 1;
	matrix[1] = 0;
	matrix[2] = 0;
	matrix[3] = 1;
	matrix[4] = 0;
	matrix[5] = 0;
	// create the scaled font
	mat[0] = m11;
	mat[1] = -m12;
	mat[2] = m21;
	mat[3] = -m22;
	m_font = m_fontEngine->getFont(fontFile, mat, matrix);

	delete fontLoc;
	if (fontsrc && !fontsrc->isFile)
		fontsrc->unref();
	return;

err2:
	delete id;
	delete fontLoc;
err1:
	if (fontsrc && !fontsrc->isFile)
		fontsrc->unref();
}



void SlaOutputDev::drawChar(GfxState *state, double x, double y, double dx, double dy, double originX, double originY, CharCode code, int nBytes, POPPLER_CONST_082 Unicode *u, int uLen)
{
//	qDebug() << "SlaOutputDev::drawChar code:" << code << "bytes:" << nBytes << "Unicode:" << u << "ulen:" << uLen << "render:" << state->getRender();
//for importing text as glyphs
if(import_text_as_vectors){
	double x1, y1, x2, y2;
	_updateFontForVectors(state);
	if (!m_font)
		return;

	// PDF 1.7 Section 9.3.6 defines eight text rendering modes.
	// 0 - Fill
	// 1 - Stroke
	// 2 - First fill and then stroke
	// 3 - Invisible
	// 4 - Fill and use as a clipping path
	// 5 - Stroke and use as a clipping path
	// 6 - First fill, then stroke and add as a clipping path
	// 7 - Only use as a clipping path.
	// TODO Implement the clipping operations. At least the characters are shown.
	int textRenderingMode = state->getRender();
	// Invisible or only used for clipping
	if (textRenderingMode == 3)
		return;
	if (textRenderingMode < 8)
	{
		SplashPath * fontPath;
		fontPath = m_font->getGlyphPath(code);
		if (fontPath)
		{
			QPainterPath qPath;
			qPath.setFillRule(Qt::WindingFill);
			for (int i = 0; i < fontPath->getLength(); ++i)
			{
				Guchar f;
				fontPath->getPoint(i, &x1, &y1, &f);
				if (f & splashPathFirst)
					qPath.moveTo(x1,y1);
				else if (f & splashPathCurve)
				{
					double x3, y3;
					++i;
					fontPath->getPoint(i, &x2, &y2, &f);
					++i;
					fontPath->getPoint(i, &x3, &y3, &f);
					qPath.cubicTo(x1,y1,x2,y2,x3,y3);
				}
				else
					qPath.lineTo(x1,y1);
				if (f & splashPathLast)
					qPath.closeSubpath();
			}
			const double *ctm = state->getCTM();
			m_ctm = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]);
			double xCoor = m_doc->currentPage()->xOffset();
			double yCoor = m_doc->currentPage()->yOffset();
			FPointArray textPath;
			textPath.fromQPainterPath(qPath);
			FPoint wh = textPath.widthHeight();
			if (textRenderingMode > 3)
			{
				QTransform mm;
				mm.scale(1, -1);
				mm.translate(x, -y);
				// Remember the glyph for later clipping
 				m_clipTextPath.addPath(m_ctm.map(mm.map(qPath)));
			}
			if ((textPath.size() > 3) && ((wh.x() != 0.0) || (wh.y() != 0.0)) && (textRenderingMode != 7))
			{
				int z = m_doc->itemAdd(PageItem::Polygon, PageItem::Unspecified, xCoor, yCoor, 10, 10, 0, CommonStrings::None, CommonStrings::None);
				PageItem* ite = m_doc->Items->at(z);
				QTransform mm;
				mm.scale(1, -1);
				mm.translate(x, -y);
				textPath.map(mm);
				textPath.map(m_ctm);
				ite->PoLine = textPath.copy();
				ite->ClipEdited = true;
				ite->FrameType = 3;
				ite->setLineEnd(PLineEnd);
				ite->setLineJoin(PLineJoin);
				ite->setTextFlowMode(PageItem::TextFlowDisabled);
				// Fill text rendering modes. See above
				if (textRenderingMode == 0 || textRenderingMode == 2 || textRenderingMode == 4 || textRenderingMode == 6)
				{
					CurrColorFill = getColor(state->getFillColorSpace(), state->getFillColor(), &CurrFillShade);
					ite->setFillColor(CurrColorFill);
					ite->setFillShade(CurrFillShade);
					ite->setFillEvenOdd(false);
					ite->setFillTransparency(1.0 - state->getFillOpacity());
					ite->setFillBlendmode(getBlendMode(state));
				}
				// Stroke text rendering modes. See above
				if (textRenderingMode == 1 || textRenderingMode == 2 || textRenderingMode == 5 || textRenderingMode == 6)
				{
					CurrColorStroke = getColor(state->getStrokeColorSpace(), state->getStrokeColor(), &CurrStrokeShade);
					ite->setLineColor(CurrColorStroke);
					ite->setLineWidth(state->getTransformedLineWidth());
					ite->setLineTransparency(1.0 - state->getStrokeOpacity());
					ite->setLineBlendmode(getBlendMode(state));
					ite->setLineShade(CurrStrokeShade);
				}
				m_doc->adjustItemSize(ite);
				m_Elements->append(ite);
				if (m_groupStack.count() != 0)
				{
					m_groupStack.top().Items.append(ite);
					applyMask(ite);
				}
				delete fontPath;
			}
		}
	}
} else {
	//for importing text as text
	addChar(state, x, y, dx, dy, originX, originY, code, nBytes, u, uLen);
}
}

GBool SlaOutputDev::beginType3Char(GfxState *state, double x, double y, double dx, double dy, CharCode code, POPPLER_CONST_082 Unicode *u, int uLen)
{
//	qDebug() << "beginType3Char";
	GfxFont *gfxFont;
	if (!(gfxFont = state->getFont()))
		return gTrue;
	if (gfxFont->getType() != fontType3)
		return gTrue;
	F3Entry f3e;
	f3e.colored = false;
	m_F3Stack.push(f3e);
	pushGroup();
	return gFalse;
}

void SlaOutputDev::endType3Char(GfxState *state)
{
//	qDebug() << "endType3Char";
	F3Entry f3e = m_F3Stack.pop();
	groupEntry gElements = m_groupStack.pop();
	m_doc->m_Selection->clear();
	if (gElements.Items.count() > 0)
	{
		m_doc->m_Selection->delaySignalsOn();
		for (int dre = 0; dre < gElements.Items.count(); ++dre)
		{
			m_doc->m_Selection->addItem(gElements.Items.at(dre), true);
			m_Elements->removeAll(gElements.Items.at(dre));
		}
		PageItem *ite;
		if (m_doc->m_Selection->count() > 1)
			ite = m_doc->groupObjectsSelection();
		else
			ite = m_doc->m_Selection->itemAt(0);
		if (!f3e.colored)
		{
			m_doc->itemSelection_SetItemBrush(CurrColorFill);
			m_doc->itemSelection_SetItemBrushShade(CurrFillShade);
			m_doc->itemSelection_SetItemFillTransparency(1.0 - state->getFillOpacity());
			m_doc->itemSelection_SetItemFillBlend(getBlendMode(state));
		}
		m_Elements->append(ite);
		m_doc->m_Selection->clear();
		m_doc->m_Selection->delaySignalsOff();
	}
}

void SlaOutputDev::type3D0(GfxState * /*state*/, double /*wx*/, double /*wy*/)
{
//	qDebug() << "type3D0";
	if (m_F3Stack.count() > 0)
		m_F3Stack.top().colored = true;
}

void SlaOutputDev::type3D1(GfxState *state, double wx, double wy, double llx, double lly, double urx, double ury)
{
//	qDebug() << "type3D1";
	if (m_F3Stack.count() > 0)
		m_F3Stack.top().colored = false;
}

void SlaOutputDev::beginTextObject(GfxState *state)
{
	// for importing text as glyphs
	pushGroup();
	//for importing text as text
	_in_text_object = true;
	_invalidated_style = true;  // Force copying of current state
}

void SlaOutputDev::endTextObject(GfxState *state)
{

	// for text as glyphs
	if(import_text_as_vectors) {
	//	qDebug() << "SlaOutputDev::endTextObject";
		if (!m_clipTextPath.isEmpty())
		{
			m_currentClipPath = intersection(m_currentClipPath, m_clipTextPath);
			m_clipTextPath = QPainterPath();
		}
		if (m_groupStack.count() != 0)
		{
			groupEntry gElements = m_groupStack.pop();
			tmpSel->clear();
			if (gElements.Items.count() > 0)
			{
				for (int dre = 0; dre < gElements.Items.count(); ++dre)
				{
					tmpSel->addItem(gElements.Items.at(dre), true);
					m_Elements->removeAll(gElements.Items.at(dre));
				}
				PageItem *ite;
				if (gElements.Items.count() != 1)
					ite = m_doc->groupObjectsSelection(tmpSel);
				else
					ite = gElements.Items.first();
				ite->setGroupClipping(false);
				ite->setFillTransparency(1.0 - state->getFillOpacity());
				ite->setFillBlendmode(getBlendMode(state));
				for (int as = 0; as < tmpSel->count(); ++as)
				{
					m_Elements->append(tmpSel->itemAt(as));
				}
				if (m_groupStack.count() != 0)
					applyMask(ite);
			}
			if (m_groupStack.count() != 0)
			{
				for (int as = 0; as < tmpSel->count(); ++as)
				{
					m_groupStack.top().Items.append(tmpSel->itemAt(as));
				}
			}
			tmpSel->clear();
		}
	}else {
	// for importing text as text
		_flushText(state);
		// TODO: clip if render_mode >= 4
		_in_text_object = false;
	}
}

QString SlaOutputDev::getColor(GfxColorSpace *color_space, POPPLER_CONST_070 GfxColor *color, int *shade)
{
	QString fNam;
	QString namPrefix = "FromPDF";
	ScColor tmp;
	tmp.setSpotColor(false);
	tmp.setRegistrationColor(false);
	*shade = 100;
	/*if (m_F3Stack.count() > 0)
	{
		if (!m_F3Stack.top().colored)
			return "Black";
	}*/

	if ((color_space->getMode() == csDeviceRGB) || (color_space->getMode() == csCalRGB))
	{
		GfxRGB rgb;
		color_space->getRGB(color, &rgb);
		double Rc = colToDbl(rgb.r);
		double Gc = colToDbl(rgb.g);
		double Bc = colToDbl(rgb.b);
		tmp.setRgbColorF(Rc, Gc, Bc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	else if (color_space->getMode() == csDeviceCMYK)
	{
		GfxCMYK cmyk;
		color_space->getCMYK(color, &cmyk);
		double Cc = colToDbl(cmyk.c);
		double Mc = colToDbl(cmyk.m);
		double Yc = colToDbl(cmyk.y);
		double Kc = colToDbl(cmyk.k);
		tmp.setCmykColorF(Cc, Mc, Yc, Kc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	else if ((color_space->getMode() == csCalGray) || (color_space->getMode() == csDeviceGray))
	{
		GfxGray gray;
		color_space->getGray(color, &gray);
		double Kc = 1.0 - colToDbl(gray);
		tmp.setCmykColorF(0, 0, 0, Kc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	else if (color_space->getMode() == csSeparation)
	{
		GfxSeparationColorSpace* sepColorSpace = (GfxSeparationColorSpace*) color_space;
		GfxColorSpace* altColorSpace = sepColorSpace->getAlt();
		QString name = QString(sepColorSpace->getName()->getCString());
		bool isRegistrationColor = (name == "All");
		if (isRegistrationColor)
		{
			tmp.setCmykColorF(1.0, 1.0, 1.0, 1.0);
			tmp.setRegistrationColor(true);
			name = "Registration";
		}
		else if ((altColorSpace->getMode() == csDeviceRGB) || (altColorSpace->getMode() == csCalRGB))
		{
			double x = 1.0;
			double comps[gfxColorMaxComps];
			sepColorSpace->getFunc()->transform(&x, comps);
			tmp.setRgbColorF(comps[0], comps[1], comps[2]);
		}
		else if ((altColorSpace->getMode() == csCalGray) || (altColorSpace->getMode() == csDeviceGray))
		{
			double x = 1.0;
			double comps[gfxColorMaxComps];
			sepColorSpace->getFunc()->transform(&x, comps);
			tmp.setCmykColorF(0.0, 0.0, 0.0, 1.0 - comps[0]);
		}
		else if (altColorSpace->getMode() == csLab)
		{
			double x = 1.0;
			double comps[gfxColorMaxComps];
			sepColorSpace->getFunc()->transform(&x, comps);
			tmp.setLabColor(comps[0], comps[1], comps[2]);
		}
		else
		{
			GfxCMYK cmyk;
			color_space->getCMYK(color, &cmyk);
			double Cc = colToDbl(cmyk.c);
			double Mc = colToDbl(cmyk.m);
			double Yc = colToDbl(cmyk.y);
			double Kc = colToDbl(cmyk.k);
			tmp.setCmykColorF(Cc, Mc, Yc, Kc);
		}
		tmp.setSpotColor(true);

		fNam = m_doc->PageColors.tryAddColor(name, tmp);
		*shade = qRound(colToDbl(color->c[0]) * 100);
	}
	else
	{
		GfxRGB rgb;
		color_space->getRGB(color, &rgb);
		double Rc = colToDbl(rgb.r);
		double Gc = colToDbl(rgb.g);
		double Bc = colToDbl(rgb.b);
		tmp.setRgbColorF(Rc, Gc, Bc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
//		qDebug() << "update fill color other colorspace" << color_space->getMode() << "treating as rgb" << Rc << Gc << Bc;
	}
	if (fNam == namPrefix+tmp.name())
		m_importedColors->append(fNam);
	return fNam;
}

QString SlaOutputDev::getAnnotationColor(const AnnotColor *color)
{
	QString fNam;
	QString namPrefix = "FromPDF";
	ScColor tmp;
	tmp.setSpotColor(false);
	tmp.setRegistrationColor(false);
	if (color->getSpace() == AnnotColor::colorTransparent)
		return CommonStrings::None;
	if (color->getSpace() == AnnotColor::colorRGB)
	{
		const double *color_data = color->getValues();
		double Rc = color_data[0];
		double Gc = color_data[1];
		double Bc = color_data[2];
		tmp.setRgbColorF(Rc, Gc, Bc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	else if (color->getSpace() == AnnotColor::colorCMYK)
	{
		const double *color_data = color->getValues();
		double Cc = color_data[0];
		double Mc = color_data[1];
		double Yc = color_data[2];
		double Kc = color_data[3];
		tmp.setCmykColorF(Cc, Mc, Yc, Kc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	else if (color->getSpace() == AnnotColor::colorGray)
	{
		const double *color_data = color->getValues();
		double Kc = 1.0 - color_data[0];
		tmp.setCmykColorF(0, 0, 0, Kc);
		fNam = m_doc->PageColors.tryAddColor(namPrefix+tmp.name(), tmp);
	}
	if (fNam == namPrefix+tmp.name())
		m_importedColors->append(fNam);
	return fNam;
}

QString SlaOutputDev::convertPath(POPPLER_CONST_083 GfxPath *path)
{
//	qDebug() << "SlaOutputDev::convertPath";
	if (! path)
		return QString();

	QString output;
	pathIsClosed = false;

	for (int i = 0; i < path->getNumSubpaths(); ++i)
	{
		POPPLER_CONST_083 GfxSubpath * subpath = path->getSubpath(i);
		if (subpath->getNumPoints() > 0)
		{
			output += QString("M %1 %2").arg(subpath->getX(0)).arg(subpath->getY(0));
			int j = 1;
			while (j < subpath->getNumPoints())
			{
				if (subpath->getCurve(j))
				{
					output += QString("C %1 %2 %3 %4 %5 %6")
					.arg(subpath->getX(j)).arg(subpath->getY(j))
					.arg(subpath->getX(j + 1)).arg(subpath->getY(j + 1))
					.arg(subpath->getX(j + 2)).arg(subpath->getY(j + 2));
					j += 3;
				}
				else
				{
					output += QString("L %1 %2").arg(subpath->getX(j)).arg(subpath->getY(j));
					++j;
				}
			}
			if (subpath->isClosed())
			{
				output += QString("Z");
				pathIsClosed = true;
			}
		}
	}
	return output;
}

void SlaOutputDev::getPenState(GfxState *state)
{
	switch (state->getLineCap())
	{
		case 0:
			PLineEnd = Qt::FlatCap;
			break;
		case 1:
			PLineEnd = Qt::RoundCap;
			break;
		case 2:
			PLineEnd = Qt::SquareCap;
			break;
	}
	switch (state->getLineJoin())
	{
		case 0:
			PLineJoin = Qt::MiterJoin;
			break;
		case 1:
			PLineJoin = Qt::RoundJoin;
			break;
		case 2:
			PLineJoin = Qt::BevelJoin;
			break;
	}
	double lw = state->getLineWidth();
	double *dashPattern;
	int dashLength;
	state->getLineDash(&dashPattern, &dashLength, &DashOffset);
	QVector<double> pattern(dashLength);
	for (int i = 0; i < dashLength; ++i)
	{
		pattern[i] = dashPattern[i] / lw;
	}
	DashValues = pattern;
}

int SlaOutputDev::getBlendMode(GfxState *state)
{
	int mode = 0;
	switch (state->getBlendMode())
	{
		default:
		case gfxBlendNormal:
			mode = 0;
			break;
		case gfxBlendDarken:
			mode = 1;
			break;
		case gfxBlendLighten:
			mode = 2;
			break;
		case gfxBlendMultiply:
			mode = 3;
			break;
		case gfxBlendScreen:
			mode = 4;
			break;
		case gfxBlendOverlay:
			mode = 5;
			break;
		case gfxBlendHardLight:
			mode = 6;
			break;
		case gfxBlendSoftLight:
			mode = 7;
			break;
		case gfxBlendDifference:
			mode = 8;
			break;
		case gfxBlendExclusion:
			mode = 9;
			break;
		case gfxBlendColorDodge:
			mode = 10;
			break;
		case gfxBlendColorBurn:
			mode = 11;
			break;
		case gfxBlendHue:
			mode = 12;
			break;
		case gfxBlendSaturation:
			mode = 13;
			break;
		case gfxBlendColor:
			mode = 14;
			break;
		case gfxBlendLuminosity:
			mode = 15;
			break;
	}
	return mode;
}

void SlaOutputDev::applyMask(PageItem *ite)
{
	if (m_groupStack.count() != 0)
	{
		if (!m_groupStack.top().maskName.isEmpty())
		{
			ite->setPatternMask(m_groupStack.top().maskName);
			QPointF maskPos = m_groupStack.top().maskPos;
			double sx, sy, px, py, r, shx, shy;
			ite->maskTransform(sx, sy, px, py, r, shx, shy);
			ite->setMaskTransform(sx, sy, maskPos.x() - ite->xPos(), maskPos.y() - ite->yPos(), r, shx, shy);
			if (m_groupStack.top().alpha)
			{
				if (m_groupStack.top().inverted)
					ite->setMaskType(8);
				else
					ite->setMaskType(3);
			}
			else
			{
				if (m_groupStack.top().inverted)
					ite->setMaskType(7);
				else
					ite->setMaskType(6);
			}
		}
	}
	// Code for updating our Progressbar, needs to be called this way, as we have no
	// possibility to get the current fileposition.
	updateGUICounter++;
	if (updateGUICounter > 20)
	{
		qApp->processEvents();
		updateGUICounter = 0;
	}
}

void SlaOutputDev::pushGroup(const QString& maskName, GBool forSoftMask, GBool alpha, bool inverted)
{
	groupEntry gElements;
	gElements.forSoftMask = forSoftMask;
	gElements.alpha = alpha;
	gElements.inverted = inverted;
	gElements.maskName = maskName;
	m_groupStack.push(gElements);
}

QString SlaOutputDev::UnicodeParsedString(POPPLER_CONST GooString *s1)
{
	if ( !s1 || s1->getLength() == 0 )
		return QString();
	GBool isUnicode;
	int i;
	Unicode u;
	QString result;
	if ((s1->getChar(0) & 0xff) == 0xfe && (s1->getLength() > 1 && (s1->getChar(1) & 0xff) == 0xff))
	{
		isUnicode = gTrue;
		i = 2;
		result.reserve((s1->getLength() - 2) / 2);
	}
	else
	{
		isUnicode = gFalse;
		i = 0;
		result.reserve(s1->getLength());
	}
	while (i < s1->getLength())
	{
		if (isUnicode)
		{
			u = ((s1->getChar(i) & 0xff) << 8) | (s1->getChar(i+1) & 0xff);
			i += 2;
		}
		else
		{
			u = s1->getChar(i) & 0xff;
			++i;
		}
		result += QChar( u );
	}
	return result;
}

QString SlaOutputDev::UnicodeParsedString(const std::string& s1)
{
	if (s1.length() == 0)
		return QString();
	GBool isUnicode;
	size_t i;
	Unicode u;
	QString result;
	if ((s1.at(0) & 0xff) == 0xfe && (s1.length() > 1 && (s1.at(1) & 0xff) == 0xff))
	{
		isUnicode = gTrue;
		i = 2;
		result.reserve((s1.length() - 2) / 2);
	}
	else
	{
		isUnicode = gFalse;
		i = 0;
		result.reserve(s1.length());
	}
	while (i < s1.length())
	{
		if (isUnicode)
		{
			u = ((s1.at(i) & 0xff) << 8) | (s1.at(i+1) & 0xff);
			i += 2;
		}
		else
		{
			u = s1.at(i) & 0xff;
			++i;
		}
		// #15616: imagemagick may write unicode strings incorrectly in PDF
		if (u == 0)
			continue;
		result += QChar(u);
	}
	return result;
}

bool SlaOutputDev::checkClip()
{
	bool ret = false;
	if (!m_currentClipPath.isEmpty())
	{
		QRectF bbox = m_currentClipPath.boundingRect();
		if ((bbox.width() > 0) && (bbox.height() > 0))
			ret = true;
	}
	return ret;
}

void PdfFont::Apply(ScribusDoc& p)
{
	//TODO: Implement or remove from header, whichever is correct for the implementation
}
