/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#ifndef nsIEditor_h__
#define nsIEditor_h__
#include "nsISupports.h"
#include "nsIDOMDocument.h"
/*
Editor interface to outside world
*/

#define NS_IEDITOR_IID \
{/* A3C5EE71-742E-11d2-8F2C-006008310194*/ \
0xa3c5ee71, 0x742e, 0x11d2, \
{0x8f, 0x2c, 0x0, 0x60, 0x8, 0x31, 0x1, 0x94} }

enum PROPERTIES{NONE = 0,BOLD = 1,NUMPROPERTIES};

/**
 * An editor specific interface. 
 * <P>
 * It's implemented by an object that has to execute specific editor
 * behavior.
 * <P>
 * It is used by the script runtime to collect information about an object
 */
class nsIEditor  : public nsISupports{
public:

  /**
   * Init tells is to tell the implementation of nsIEditor to begin its services
   * @param aDomInterface The dom interface being observed
   */
  virtual nsresult Init(nsIDOMDocument *aDomInterface) = 0;

  /**
   * GetDomInterface() WILL NOT RETURN AN ADDREFFED POINTER
   *
   * @param aDomInterface The dom interface being observed
   */
  virtual nsresult GetDomInterface(nsIDOMDocument **)=0;

  /**
   * SetProperties() sets the properties of the current selection
   *
   * @param aProperty An enum that lists the various properties that can be applied, bold, ect.
   */
  virtual nsresult SetProperties(PROPERTIES aProperty)=0;

  /**
   * SetProperties() sets the properties of the current selection
   *
   * @param aProperty An enum that will recieve the various properties that can be applied from the current selection.
   */
  virtual nsresult GetProperties(PROPERTIES &aProperty)=0;
};

/*Factory Method to create an Editor*/

extern nsresult NS_InitEditor(nsIEditor **, nsIDOMDocument *);

#endif //nsIEditor_h__

